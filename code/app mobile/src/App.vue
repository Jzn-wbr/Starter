<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import {
  Activity,
  AlertTriangle,
  Battery,
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
  ShieldAlert,
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

const activeTab = ref<TabName>('alarm')
const loading = ref(true)
const saving = ref(false)
const uploading = ref(false)
const refreshing = ref(false)
const errorMessage = ref('')
const successMessage = ref('')
const detailsOpen = ref(false)
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

const selectedSlot = computed(() => slots.value[selectedSlotIndex.value] ?? slots.value[0])
const selectedTrack = computed(() => {
  const selectedId = audioSelection.value?.selected_track_id
  return musicTracks.value.find((track) => track.id === selectedId) ?? null
})
const selectedAudioLabel = computed(() => {
  if (audioSelection.value?.audio_source === 'fallback') return 'Son de secours local'
  return selectedTrack.value?.title ?? 'Aucune musique sélectionnée'
})
const hasProblem = computed(() => {
  return stationStatus.value?.problem_code !== 'none' || braceletStatus.value?.problem_code !== 'none'
})
const stationStale = computed(() => isStale(stationStatus.value?.updated_at))
const braceletStale = computed(() => isStale(braceletStatus.value?.updated_at))
const canUseSupabase = computed(() => SUPABASE_CONFIGURED)

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

async function chooseFallback() {
  await updateAudioSelection(null)
}

async function chooseTrack(track: MusicTrack) {
  await updateAudioSelection(track.id)
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
    if (!['audio/mpeg', 'audio/wav', 'audio/ogg', 'audio/mp4'].includes(file.type)) {
      throw new Error('Format non accepté. Utilise MP3, WAV, OGG ou MP4 audio.')
    }

    const client = requireSupabase()
    const safeName = file.name.replace(/[^a-zA-Z0-9._-]/g, '-')
    const storagePath = `music/${crypto.randomUUID()}-${safeName}`
    const { error: uploadError } = await client.storage.from('wake-up-music').upload(storagePath, file)

    if (uploadError) throw uploadError

    const { data: publicUrlData } = client.storage.from('wake-up-music').getPublicUrl(storagePath)
    const { error: insertError } = await client.from('music_tracks').insert({
      title: file.name.replace(/\.[^/.]+$/, ''),
      storage_path: storagePath,
      public_url: publicUrlData.publicUrl,
      is_available: true,
    })

    if (insertError) throw insertError

    successMessage.value = 'Musique ajoutée.'
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

          <div class="time-wheel" aria-label="Roue horaire des 20 prochaines heures">
            <button class="wheel-step" type="button" :disabled="selectedSlotIndex === 0" @click="selectOffset(-1)">
              −15 min
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
              +15 min
            </button>
          </div>

          <input v-model.number="selectedSlotIndex" class="slot-range" type="range" min="0" :max="slots.length - 1" />
          <div class="range-labels">
            <span>Maintenant</span>
            <span>+20 h</span>
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
            <button class="status-card" type="button" @click="detailsOpen = !detailsOpen">
              <Radio :size="22" />
              <span>Station</span>
              <strong>{{ stationStatus ? stationStateLabels[stationStatus.station_state] : 'Inconnue' }}</strong>
              <small v-if="stationStale">Statut ancien</small>
            </button>

            <button class="status-card" type="button" @click="detailsOpen = !detailsOpen">
              <Watch :size="22" />
              <span>Bracelet</span>
              <strong>{{ braceletStatus ? braceletStateLabels[braceletStatus.bracelet_state] : 'Inconnu' }}</strong>
              <small v-if="braceletStale">Statut ancien</small>
            </button>

            <button class="status-card" type="button" @click="detailsOpen = !detailsOpen">
              <BatteryCharging v-if="braceletStatus?.bracelet_state === 'charging'" :size="22" />
              <Battery v-else :size="22" />
              <span>Batterie</span>
              <strong>
                {{
                  braceletStatus?.bracelet_battery_percent === null || braceletStatus?.bracelet_battery_percent === undefined
                    ? 'Inconnue'
                    : `${braceletStatus.bracelet_battery_percent}%`
                }}
              </strong>
            </button>

            <button class="status-card" type="button" @click="detailsOpen = !detailsOpen">
              <ShieldAlert v-if="hasProblem" :size="22" />
              <Activity v-else :size="22" />
              <span>Diagnostic</span>
              <strong>{{ hasProblem ? 'À vérifier' : 'Normal' }}</strong>
            </button>
          </div>

          <div v-if="detailsOpen" class="details-panel">
            <div>
              <span>Station</span>
              <strong>{{ stationStatus ? problemLabels[stationStatus.problem_code] : 'Aucune donnée' }}</strong>
              <small>{{ stationStatus?.problem_message || 'Pas de message' }}</small>
              <small>Revision active: {{ stationStatus?.active_alarm_revision ?? 'n/a' }}</small>
            </div>
            <div>
              <span>Bracelet</span>
              <strong>{{ braceletStatus ? problemLabels[braceletStatus.problem_code] : 'Aucune donnée' }}</strong>
              <small>{{ braceletStatus?.problem_message || 'Pas de message' }}</small>
              <small>Dernier paquet: {{ braceletStatus?.bracelet_last_seen_ms ?? 'n/a' }} ms</small>
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
