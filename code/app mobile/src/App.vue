<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import {
  AlertTriangle,
  BatteryCharging,
  Bell,
  Check,
  ChevronRight,
  CircleAlert,
  Clock3,
  CloudOff,
  Headphones,
  Loader2,
  Music,
  Pause,
  Play,
  Plus,
  Radio,
  Smartphone,
  Trash2,
  Volume2,
  Watch,
} from 'lucide-vue-next'
import type { RealtimeChannel } from '@supabase/supabase-js'
import {
  type AlarmAudioSelection,
  type AlarmPlan,
  type BraceletStatus,
  type MusicTrack,
  type ProblemCode,
  type StationStatus,
  SUPABASE_CONFIGURED,
  requireSupabase,
} from './supabase'
import { buildAlarmSlots, formatRelativeHours, pickInitialSlot } from './timeWindow'

type TabName = 'alarm' | 'music'
type StatusTarget = 'station' | 'bracelet'

const BATTERY_EMPTY_V = 3.3
const BATTERY_FULL_V = 4.2
const ACCEPTED_AUDIO_TYPES = new Set(['audio/mpeg', 'audio/wav', 'audio/ogg', 'audio/mp4'])
const ACCEPTED_AUDIO_EXTENSIONS = new Set(['mp3', 'wav', 'ogg', 'm4a', 'mp4'])

const activeTab = ref<TabName>('alarm')
const loading = ref(true)
const saving = ref(false)
const uploading = ref(false)
const refreshing = ref(false)
const errorMessage = ref('')
const successMessage = ref('')
const selectedStatusDetails = ref<StatusTarget | null>(null)
const playingTrackId = ref<string | null>(null)
const audioElement = ref<HTMLAudioElement | null>(null)

const alarmPlan = ref<AlarmPlan | null>(null)
const audioSelection = ref<AlarmAudioSelection | null>(null)
const musicTracks = ref<MusicTrack[]>([])
const stationStatus = ref<StationStatus | null>(null)
const braceletStatus = ref<BraceletStatus | null>(null)

const slots = ref(buildAlarmSlots())
const selectedSlotIndex = ref(0)
const draftVolume = ref(70)

let pollTimer: ReturnType<typeof window.setInterval> | undefined
let slotTimer: ReturnType<typeof window.setInterval> | undefined
let realtimeChannel: RealtimeChannel | undefined

type PreparedAudioFile = {
  file: File
  strippedBytes: number
}

const selectedSlot = computed(() => slots.value[selectedSlotIndex.value] ?? slots.value[0])
const selectedTrack = computed(() => {
  const selectedId = audioSelection.value?.selected_track_id
  return musicTracks.value.find((track) => track.id === selectedId) ?? null
})
const selectedAudioLabel = computed(() => {
  if (audioSelection.value?.audio_source === 'fallback') return 'Son de secours local'
  return selectedTrack.value?.title ?? 'Aucune musique sélectionnée'
})
const configuredAlarmLabel = computed(() => {
  if (!alarmPlan.value?.alarm_date || !alarmPlan.value?.alarm_time) return 'Aucune heure enregistrée'
  const date = new Date(`${alarmPlan.value.alarm_date}T${alarmPlan.value.alarm_time}`)
  return `${date.toLocaleDateString('fr-CH', { weekday: 'long', day: '2-digit', month: 'long' })} à ${date.toLocaleTimeString('fr-CH', { hour: '2-digit', minute: '2-digit', hour12: false })}`
})
const alarmIsActive = computed(() => Boolean(alarmPlan.value?.enabled))
const hasProblem = computed(() => {
  return stationStatus.value?.problem_code !== 'none' || braceletStatus.value?.problem_code !== 'none'
})
const stationStale = computed(() => isStale(stationStatus.value?.updated_at))
const braceletStale = computed(() => isStale(braceletStatus.value?.updated_at))
const canUseSupabase = computed(() => SUPABASE_CONFIGURED)
const stationBatteryPercent = computed(() => estimateBatteryPercent(stationStatus.value?.station_battery_voltage))
const braceletBatteryPercent = computed(() => estimateBatteryPercent(braceletStatus.value?.bracelet_battery_voltage))
const stationStateLabels: Record<string, string> = {
  idle: 'Au repos',
  armed: 'Armée',
  ringing: 'Sonnerie',
  validating_activity: 'Validation activité',
  stopped: 'Arrêtée',
  fault: 'Défaut',
}

const braceletStateLabels: Record<string, string> = {
  unknown: 'Inconnu',
  charging: 'En charge',
  ready: 'Prêt',
  active: 'Actif',
  validated: 'Validé',
  low_battery: 'Batterie faible',
  fault: 'Défaut',
}

const problemLabels: Record<ProblemCode, string> = {
  none: 'Aucun problème',
  wifi_unavailable: 'WiFi indisponible',
  supabase_unavailable: 'Supabase indisponible',
  time_unknown: 'Heure inconnue',
  no_valid_alarm_config: 'Configuration invalide',
  music_stream_failed: 'Lecture musique échouée',
  fallback_audio_failed: 'Son de secours échoué',
  bracelet_missing: 'Bracelet absent',
  bracelet_low_battery: 'Bracelet faible',
  bracelet_fault: 'Défaut bracelet',
  sensor_fault: 'Défaut capteur',
  audio_fault: 'Défaut audio',
  unknown_fault: 'Défaut inconnu',
}

onMounted(() => {
  loadInitialData()
  startStatusPolling()
  startRealtimeUpdates()
  document.addEventListener('visibilitychange', handleVisibilityChange)
  slotTimer = window.setInterval(refreshSlots, 60_000)
})

onBeforeUnmount(() => {
  document.removeEventListener('visibilitychange', handleVisibilityChange)
  stopStatusPolling()
  stopRealtimeUpdates()
  if (slotTimer) window.clearInterval(slotTimer)
  stopPreview()
})

function isPageVisible() {
  return document.visibilityState === 'visible'
}

function startStatusPolling() {
  if (pollTimer || !isPageVisible()) return
  pollTimer = window.setInterval(loadStatuses, 5_000)
}

function stopStatusPolling() {
  if (!pollTimer) return
  window.clearInterval(pollTimer)
  pollTimer = undefined
}

function handleVisibilityChange() {
  if (!isPageVisible()) {
    stopStatusPolling()
    return
  }

  loadInitialData()
  startStatusPolling()
}

function startRealtimeUpdates() {
  if (!SUPABASE_CONFIGURED || realtimeChannel) return

  const client = requireSupabase()
  realtimeChannel = client
    .channel('starter-pwa-main')
    .on('postgres_changes', { event: '*', schema: 'public', table: 'alarm_plan', filter: 'id=eq.main' }, () => {
      void loadAlarmConfig()
    })
    .on('postgres_changes', { event: '*', schema: 'public', table: 'alarm_audio_selection', filter: 'id=eq.main' }, () => {
      void loadAlarmConfig()
    })
    .on('postgres_changes', { event: '*', schema: 'public', table: 'station_status', filter: 'id=eq.main' }, (payload) => {
      stationStatus.value = payload.new as StationStatus
    })
    .on('postgres_changes', { event: '*', schema: 'public', table: 'bracelet_status', filter: 'id=eq.main' }, (payload) => {
      braceletStatus.value = payload.new as BraceletStatus
    })
    .on('postgres_changes', { event: '*', schema: 'public', table: 'music_tracks' }, () => {
      void loadMusicTracks()
    })
    .subscribe()
}

function stopRealtimeUpdates() {
  if (!realtimeChannel) return
  const client = requireSupabase()
  void client.removeChannel(realtimeChannel)
  realtimeChannel = undefined
}

async function loadInitialData() {
  loading.value = true
  try {
    await Promise.all([loadAlarmConfig(), loadMusicTracks(), loadStatuses()])
  } catch (error) {
    showError(error)
  } finally {
    loading.value = false
  }
}

async function loadAlarmConfig() {
  if (!SUPABASE_CONFIGURED) return
  const client = requireSupabase()
  const [planResult, audioResult] = await Promise.all([
    client.from('alarm_plan').select('*').eq('id', 'main').maybeSingle(),
    client.from('alarm_audio_selection').select('*').eq('id', 'main').maybeSingle(),
  ])

  if (planResult.error) throw planResult.error
  if (audioResult.error) throw audioResult.error

  alarmPlan.value = planResult.data as AlarmPlan | null
  audioSelection.value = audioResult.data as AlarmAudioSelection | null
  draftVolume.value = audioSelection.value?.volume_percent ?? 70
  selectedSlotIndex.value = pickInitialSlot(slots.value, alarmPlan.value?.alarm_date ?? null, alarmPlan.value?.alarm_time ?? null)
}

async function loadMusicTracks() {
  if (!SUPABASE_CONFIGURED) return
  const client = requireSupabase()
  const { data, error } = await client
    .from('music_tracks')
    .select('*')
    .eq('is_available', true)
    .order('created_at', { ascending: false })

  if (error) throw error
  musicTracks.value = (data ?? []) as MusicTrack[]
}

async function loadStatuses() {
  if (!SUPABASE_CONFIGURED || !isPageVisible()) return
  refreshing.value = true
  try {
    const client = requireSupabase()
    const [stationResult, braceletResult] = await Promise.all([
      client.from('station_status').select('*').eq('id', 'main').maybeSingle(),
      client.from('bracelet_status').select('*').eq('id', 'main').maybeSingle(),
    ])

    if (stationResult.error) throw stationResult.error
    if (braceletResult.error) throw braceletResult.error

    stationStatus.value = stationResult.data as StationStatus | null
    braceletStatus.value = braceletResult.data as BraceletStatus | null
  } catch (error) {
    showError(error)
  } finally {
    refreshing.value = false
  }
}

async function saveAlarm() {
  if (!selectedSlot.value || !audioSelection.value) return
  saving.value = true
  clearMessages()

  try {
    const client = requireSupabase()
    const nextRevision = (alarmPlan.value?.revision ?? 1) + 1

    const { data: planData, error: planError } = await client
      .from('alarm_plan')
      .upsert({
        id: 'main',
        enabled: true,
        alarm_date: selectedSlot.value.isoDate,
        alarm_time: selectedSlot.value.sqlTime,
        timezone: 'Europe/Zurich',
        revision: nextRevision,
      })
      .select()
      .single()

    if (planError) throw planError

    const { data: audioData, error: audioError } = await client
      .from('alarm_audio_selection')
      .upsert({
        id: 'main',
        audio_source: audioSelection.value.audio_source,
        selected_track_id: audioSelection.value.selected_track_id,
        volume_percent: Number(draftVolume.value),
      })
      .select()
      .single()

    if (audioError) throw audioError

    alarmPlan.value = planData as AlarmPlan
    audioSelection.value = audioData as AlarmAudioSelection
    successMessage.value = 'Alarme enregistrée.'
  } catch (error) {
    showError(error)
  } finally {
    saving.value = false
  }
}

async function toggleAlarmEnabled() {
  if (!alarmPlan.value || !canUseSupabase.value) return
  clearMessages()
  saving.value = true
  try {
    const client = requireSupabase()
    const { data, error } = await client
      .from('alarm_plan')
      .upsert({
        ...alarmPlan.value,
        enabled: !alarmPlan.value.enabled,
        revision: (alarmPlan.value.revision ?? 1) + 1,
      })
      .select()
      .single()

    if (error) throw error
    alarmPlan.value = data as AlarmPlan
    successMessage.value = alarmPlan.value.enabled ? 'Alarme activée.' : 'Alarme désactivée.'
  } catch (error) {
    showError(error)
  } finally {
    saving.value = false
  }
}

async function chooseFallback() {
  await updateAudioSelection(null)
}

async function chooseTrack(track: MusicTrack) {
  await updateAudioSelection(track.id)
}

function audioExtension(fileName: string) {
  return fileName.split('.').pop()?.toLowerCase() ?? ''
}

function isAcceptedAudioFile(file: File) {
  return ACCEPTED_AUDIO_TYPES.has(file.type) || ACCEPTED_AUDIO_EXTENSIONS.has(audioExtension(file.name))
}

function isMp3File(file: File) {
  return file.type === 'audio/mpeg' || audioExtension(file.name) === 'mp3'
}

function readSynchsafeSize(bytes: Uint8Array, offset: number) {
  if ([bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]].some((value) => value & 0x80)) {
    return -1
  }

  return (bytes[offset] << 21) | (bytes[offset + 1] << 14) | (bytes[offset + 2] << 7) | bytes[offset + 3]
}

async function prepareAudioFileForUpload(file: File): Promise<PreparedAudioFile> {
  if (!isMp3File(file)) {
    return { file, strippedBytes: 0 }
  }

  const buffer = await file.arrayBuffer()
  const bytes = new Uint8Array(buffer)
  let start = 0
  let end = bytes.length

  while (start + 10 <= end && bytes[start] === 0x49 && bytes[start + 1] === 0x44 && bytes[start + 2] === 0x33) {
    const tagSize = readSynchsafeSize(bytes, start + 6)
    const hasFooter = (bytes[start + 5] & 0x10) !== 0
    const fullTagSize = tagSize >= 0 ? 10 + tagSize + (hasFooter ? 10 : 0) : -1

    if (fullTagSize <= 10 || start + fullTagSize > end) {
      throw new Error('En-tête MP3 ID3 invalide. Réencode le fichier avant de l’ajouter.')
    }

    start += fullTagSize
  }

  if (end - start >= 128 && bytes[end - 128] === 0x54 && bytes[end - 127] === 0x41 && bytes[end - 126] === 0x47) {
    end -= 128
  }

  if (start === 0 && end === bytes.length) {
    return { file, strippedBytes: 0 }
  }

  if (start >= end) {
    throw new Error('Le nettoyage a retiré tout le fichier MP3. Réencode le fichier avant de l’ajouter.')
  }

  const cleanedFile = new File([buffer.slice(start, end)], file.name, {
    type: 'audio/mpeg',
    lastModified: Date.now(),
  })

  return { file: cleanedFile, strippedBytes: bytes.length - (end - start) }
}

async function updateAudioSelection(trackId: string | null) {
  clearMessages()
  try {
    const client = requireSupabase()
    const nextRevision = (alarmPlan.value?.revision ?? 1) + 1
    const audioPayload = {
      id: 'main',
      audio_source: trackId ? 'track' : 'fallback',
      selected_track_id: trackId,
      volume_percent: Number(draftVolume.value),
    }

    const [audioResult, planResult] = await Promise.all([
      client.from('alarm_audio_selection').upsert(audioPayload).select().single(),
      client
        .from('alarm_plan')
        .upsert({
          id: 'main',
          enabled: alarmPlan.value?.enabled ?? false,
          alarm_date: alarmPlan.value?.alarm_date ?? null,
          alarm_time: alarmPlan.value?.alarm_time ?? null,
          timezone: alarmPlan.value?.timezone ?? 'Europe/Zurich',
          revision: nextRevision,
        })
        .select()
        .single(),
    ])

    if (audioResult.error) throw audioResult.error
    if (planResult.error) throw planResult.error

    audioSelection.value = audioResult.data as AlarmAudioSelection
    alarmPlan.value = planResult.data as AlarmPlan
    successMessage.value = trackId ? 'Musique sélectionnée.' : 'Son de secours sélectionné.'
  } catch (error) {
    showError(error)
  }
}

async function uploadMusic(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file) return

  clearMessages()
  uploading.value = true

  try {
    if (!isAcceptedAudioFile(file)) {
      throw new Error('Format non accepté. Utilise MP3, WAV, OGG ou MP4 audio.')
    }

    const preparedAudio = await prepareAudioFileForUpload(file)
    const client = requireSupabase()
    const safeName = file.name.replace(/[^a-zA-Z0-9._-]/g, '-')
    const storagePath = `music/${crypto.randomUUID()}-${safeName}`
    const { error: uploadError } = await client.storage.from('wake-up-music').upload(storagePath, preparedAudio.file, {
      contentType: preparedAudio.file.type || file.type || 'application/octet-stream',
    })

    if (uploadError) throw uploadError

    const { data: publicUrlData } = client.storage.from('wake-up-music').getPublicUrl(storagePath)
    const { error: insertError } = await client.from('music_tracks').insert({
      title: file.name.replace(/\.[^/.]+$/, ''),
      storage_path: storagePath,
      public_url: publicUrlData.publicUrl,
      is_available: true,
    })

    if (insertError) throw insertError

    successMessage.value = preparedAudio.strippedBytes > 0
      ? 'Musique ajoutée après nettoyage des métadonnées MP3.'
      : 'Musique ajoutée.'
    input.value = ''
    await loadMusicTracks()
  } catch (error) {
    showError(error)
  } finally {
    uploading.value = false
  }
}

async function deleteTrack(track: MusicTrack) {
  clearMessages()
  stopPreview()

  try {
    const client = requireSupabase()
    const { error: storageError } = await client.storage.from('wake-up-music').remove([track.storage_path])
    if (storageError) throw storageError

    const { error: deleteError } = await client.from('music_tracks').delete().eq('id', track.id)
    if (deleteError) throw deleteError

    if (audioSelection.value?.selected_track_id === track.id) {
      await chooseFallback()
    }

    await loadMusicTracks()
    successMessage.value = 'Musique supprimée.'
  } catch (error) {
    showError(error)
  }
}

function previewTrack(track: MusicTrack) {
  if (playingTrackId.value === track.id) {
    stopPreview()
    return
  }

  stopPreview()
  const audio = new Audio(track.public_url)
  audioElement.value = audio
  playingTrackId.value = track.id
  audio.addEventListener('ended', stopPreview)
  audio.play().catch((error: unknown) => {
    stopPreview()
    showError(error)
  })
}

function stopPreview() {
  if (audioElement.value) {
    audioElement.value.pause()
    audioElement.value = null
  }
  playingTrackId.value = null
}

function refreshSlots() {
  const previous = selectedSlot.value
  slots.value = buildAlarmSlots()
  const newIndex = slots.value.findIndex((slot) => slot.isoDate === previous?.isoDate && slot.sqlTime === previous?.sqlTime)
  selectedSlotIndex.value = newIndex >= 0 ? newIndex : 0
}

function selectOffset(offset: number) {
  const next = selectedSlotIndex.value + offset
  if (next >= 0 && next < slots.value.length) {
    selectedSlotIndex.value = next
  }
}

function isStale(updatedAt?: string) {
  if (!updatedAt) return true
  return Date.now() - new Date(updatedAt).getTime() > 90_000
}

function clearMessages() {
  errorMessage.value = ''
  successMessage.value = ''
}

function showError(error: unknown) {
  const message = error instanceof Error ? error.message : 'Erreur inconnue.'
  errorMessage.value = message
}

function toggleStatusDetails(target: StatusTarget) {
  selectedStatusDetails.value = selectedStatusDetails.value === target ? null : target
}

function formatBatteryPercent(value?: number | null) {
  return value === null || value === undefined ? 'Inconnue' : `${value}%`
}

function formatBatteryVoltage(value?: number | null) {
  return value === null || value === undefined ? 'Inconnue' : `${value.toFixed(2)} V`
}

function estimateBatteryPercent(voltage?: number | null) {
  if (voltage === null || voltage === undefined || voltage <= 0.1) return null
  const ratio = (voltage - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V)
  return Math.round(Math.min(1, Math.max(0, ratio)) * 100)
}

function formatUpdatedAt(value?: string) {
  if (!value) return 'Inconnu'
  return new Date(value).toLocaleString('fr-CH', {
    day: '2-digit',
    month: '2-digit',
    year: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  })
}
</script>

<template>
  <main class="app-shell">
    <section class="hero-strip">
      <div class="brand-mark">
        <Bell :size="24" />
      </div>
      <div>
        <p class="eyebrow">Réveil actif</p>
        <h1>Starter</h1>
      </div>
      <span class="status-dot" :class="{ alert: refreshing || hasProblem || stationStale || braceletStale }"></span>
    </section>

    <section v-if="!canUseSupabase" class="notice danger">
      <CloudOff :size="19" />
      <span>Supabase n’est pas configuré. Crée `.env.local` depuis `.env.example`.</span>
    </section>

    <section v-if="errorMessage" class="notice danger">
      <CircleAlert :size="19" />
      <span>{{ errorMessage }}</span>
    </section>

    <section v-if="successMessage" class="notice success">
      <Check :size="19" />
      <span>{{ successMessage }}</span>
    </section>

    <section v-if="loading" class="loading-panel">
      <Loader2 class="spin" :size="28" />
      <span>Chargement de Starter</span>
    </section>

    <template v-else>
      <section v-if="activeTab === 'alarm'" class="page-stack">
        <article class="panel alarm-panel">
          <div class="section-title">
            <div>
              <p class="eyebrow">Prochaine alarme</p>
              <h2>{{ selectedSlot?.label }}</h2>
            </div>
            <span class="soft-pill">{{ selectedSlot ? formatRelativeHours(selectedSlot.hoursFromNow) : 'Aucun créneau' }}</span>
          </div>
          <div class="alarm-summary" :class="{ inactive: !alarmIsActive }">
            <div>
              <p>Alarme fixée</p>
              <strong>{{ configuredAlarmLabel }}</strong>
            </div>
            <button class="toggle-alarm" type="button" :disabled="saving || !canUseSupabase" @click="toggleAlarmEnabled">
              <span class="dot" :class="{ off: !alarmIsActive }"></span>
              {{ alarmIsActive ? 'Active' : 'Inactive' }}
            </button>
          </div>

          <div class="time-wheel" aria-label="Roue horaire des 12 prochaines heures">
            <button class="wheel-step" type="button" :disabled="selectedSlotIndex === 0" @click="selectOffset(-1)">
              -5 min
            </button>
            <div class="wheel-face">
              <span class="wheel-orbit orbit-one"></span>
              <span class="wheel-orbit orbit-two"></span>
              <Clock3 class="wheel-icon" :size="28" />
              <strong>{{ selectedSlot?.shortLabel }}</strong>
              <small>{{ selectedSlot?.isoDate }}</small>
            </div>
            <button
              class="wheel-step"
              type="button"
              :disabled="selectedSlotIndex >= slots.length - 1"
              @click="selectOffset(1)"
            >
              +5 min
            </button>
          </div>

          <input v-model.number="selectedSlotIndex" class="slot-range" type="range" min="0" :max="slots.length - 1" />
          <div class="range-labels">
            <span>Maintenant</span>
            <span>+12 h</span>
          </div>

          <label class="volume-control">
            <span><Volume2 :size="18" /> Volume</span>
            <strong>{{ draftVolume }}%</strong>
            <input v-model.number="draftVolume" type="range" min="0" max="100" step="5" />
          </label>

          <div class="selected-audio">
            <Headphones :size="20" />
            <span>{{ selectedAudioLabel }}</span>
          </div>

          <button class="primary-action" type="button" :disabled="saving || !canUseSupabase" @click="saveAlarm">
            <Loader2 v-if="saving" class="spin" :size="18" />
            <Bell v-else :size="18" />
            Enregistrer l’alarme
          </button>
        </article>

        <article class="panel">
          <div class="section-title compact">
            <div>
              <p class="eyebrow">Système</p>
              <h2>Boîte HP et bracelet</h2>
            </div>
            <span class="status-dot" :class="{ alert: hasProblem || stationStale || braceletStale }"></span>
          </div>

          <div class="status-grid">
            <button
              class="status-card device-status-card"
              :class="{ selected: selectedStatusDetails === 'station' }"
              type="button"
              @click="toggleStatusDetails('station')"
            >
              <Radio :size="22" />
              <span>Station</span>
              <strong>{{ stationStatus ? stationStateLabels[stationStatus.station_state] : 'Inconnue' }}</strong>
              <small>État</small>
              <strong class="battery-value">{{ formatBatteryPercent(stationBatteryPercent) }}</strong>
              <small>Batterie</small>
              <small v-if="stationStale">Statut ancien</small>
            </button>

            <button
              class="status-card device-status-card"
              :class="{ selected: selectedStatusDetails === 'bracelet' }"
              type="button"
              @click="toggleStatusDetails('bracelet')"
            >
              <BatteryCharging v-if="braceletStatus?.bracelet_state === 'charging'" :size="22" />
              <Watch v-else :size="22" />
              <span>Bracelet</span>
              <strong>{{ braceletStatus ? braceletStateLabels[braceletStatus.bracelet_state] : 'Inconnu' }}</strong>
              <small>État</small>
              <strong class="battery-value">{{ formatBatteryPercent(braceletBatteryPercent) }}</strong>
              <small>Batterie</small>
              <small v-if="braceletStale">Statut ancien</small>
            </button>
          </div>

          <div v-if="selectedStatusDetails" class="details-panel">
            <div v-if="selectedStatusDetails === 'station'">
              <span>Station</span>
              <strong>{{ stationStatus ? problemLabels[stationStatus.problem_code] : 'Aucune donnée' }}</strong>
              <small>Message d'erreur: {{ stationStatus?.problem_message || 'Pas de message' }}</small>
              <small>Last update at: {{ formatUpdatedAt(stationStatus?.updated_at) }}</small>
              <small>Tension batterie: {{ formatBatteryVoltage(stationStatus?.station_battery_voltage) }}</small>
            </div>
            <div v-if="selectedStatusDetails === 'bracelet'">
              <span>Bracelet</span>
              <strong>{{ braceletStatus ? problemLabels[braceletStatus.problem_code] : 'Aucune donnée' }}</strong>
              <small>Message d'erreur: {{ braceletStatus?.problem_message || 'Pas de message' }}</small>
              <small>Last update at: {{ formatUpdatedAt(braceletStatus?.updated_at) }}</small>
              <small>Tension batterie: {{ formatBatteryVoltage(braceletStatus?.bracelet_battery_voltage) }}</small>
            </div>
          </div>
        </article>
      </section>

      <section v-else class="page-stack">
        <article class="panel">
          <div class="section-title">
            <div>
              <p class="eyebrow">Audio</p>
              <h2>Bibliothèque</h2>
            </div>
            <label class="upload-button">
              <Plus :size="18" />
              <span>{{ uploading ? 'Ajout...' : 'Ajouter' }}</span>
              <input type="file" accept="audio/mpeg,audio/wav,audio/ogg,audio/mp4" :disabled="uploading" @change="uploadMusic" />
            </label>
          </div>

          <button class="fallback-row" type="button" @click="chooseFallback">
            <div class="track-icon fallback">
              <AlertTriangle :size="20" />
            </div>
            <div>
              <strong>Son de secours local</strong>
              <span>Utilisé sans dépendre du réseau au réveil.</span>
            </div>
            <Check v-if="audioSelection?.audio_source === 'fallback'" :size="20" />
            <ChevronRight v-else :size="20" />
          </button>

          <div class="track-list">
            <article v-for="track in musicTracks" :key="track.id" class="track-row">
              <div class="track-icon">
                <Music :size="20" />
              </div>
              <div class="track-main">
                <strong>{{ track.title }}</strong>
                <span>{{ track.is_available ? 'Disponible' : 'Indisponible' }}</span>
              </div>
              <button class="icon-button small" type="button" :aria-label="`Écouter ${track.title}`" @click="previewTrack(track)">
                <Pause v-if="playingTrackId === track.id" :size="17" />
                <Play v-else :size="17" />
              </button>
              <button class="icon-button small" type="button" :aria-label="`Choisir ${track.title}`" @click="chooseTrack(track)">
                <Check v-if="audioSelection?.selected_track_id === track.id" :size="17" />
                <Headphones v-else :size="17" />
              </button>
              <button class="icon-button small danger-button" type="button" :aria-label="`Supprimer ${track.title}`" @click="deleteTrack(track)">
                <Trash2 :size="17" />
              </button>
            </article>
          </div>

          <div v-if="musicTracks.length === 0" class="empty-state">
            <Smartphone :size="26" />
            <strong>Aucune musique ajoutée</strong>
            <span>Ajoute un fichier audio pour remplacer le son de secours.</span>
          </div>
        </article>
      </section>
    </template>

    <nav class="bottom-nav" aria-label="Navigation principale">
      <button type="button" :class="{ active: activeTab === 'alarm' }" @click="activeTab = 'alarm'">
        <Bell :size="21" />
        <span>Alarme</span>
      </button>
      <button type="button" :class="{ active: activeTab === 'music' }" @click="activeTab = 'music'">
        <Music :size="21" />
        <span>Musique</span>
      </button>
    </nav>
  </main>
</template>
