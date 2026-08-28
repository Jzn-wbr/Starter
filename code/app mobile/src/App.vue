<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import {
  AlertTriangle,
  BatteryCharging,
  Bell,
  Check,
  ChevronDown,
  ChevronRight,
  CircleAlert,
  CloudOff,
  Ellipsis,
  Loader2,
  LockKeyhole,
  Music,
  Pause,
  Play,
  Plus,
  Radio,
  ShieldCheck,
  Trash2,
  Volume2,
  Watch,
  X,
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
import { buildAlarmSlots, formatRelativeHours, pickInitialSlot, type AlarmSlot } from './timeWindow'

type TabName = 'alarm' | 'music' | 'devices'

type PreparedAudioFile = {
  file: File
  strippedBytes: number
}

type HourGroup = {
  key: string
  hour: string
  slots: AlarmSlot[]
}

const BATTERY_EMPTY_V = 3.3
const BATTERY_FULL_V = 4.2
const ALARM_LOCK_BEFORE_MS = 60 * 60 * 1000
const ALARM_WINDOW_MS = 15 * 60 * 1000
const STATUS_POLL_MS = 30_000
const ACCEPTED_AUDIO_TYPES = new Set(['audio/mpeg', 'audio/wav', 'audio/ogg', 'audio/mp4'])
const ACCEPTED_AUDIO_EXTENSIONS = new Set(['mp3', 'wav', 'ogg', 'm4a', 'mp4'])

const activeTab = ref<TabName>('alarm')
const loading = ref(true)
const saving = ref(false)
const uploading = ref(false)
const refreshing = ref(false)
const errorMessage = ref('')
const successMessage = ref('')
const playingTrackId = ref<string | null>(null)
const openTrackMenuId = ref<string | null>(null)
const technicalDetailsOpen = ref(false)
const nowTick = ref(Date.now())
const audioElement = ref<HTMLAudioElement | null>(null)
const fileInput = ref<HTMLInputElement | null>(null)
const hourWheel = ref<HTMLElement | null>(null)
const minuteWheel = ref<HTMLElement | null>(null)

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
let hourScrollTimer: ReturnType<typeof window.setTimeout> | undefined
let minuteScrollTimer: ReturnType<typeof window.setTimeout> | undefined

const selectedSlot = computed(() => slots.value[selectedSlotIndex.value] ?? slots.value[0])
const hourGroups = computed<HourGroup[]>(() => {
  const groups = new Map<string, HourGroup>()

  for (const slot of slots.value) {
    const key = `${slot.isoDate}-${slot.date.getHours()}`
    if (!groups.has(key)) {
      groups.set(key, {
        key,
        hour: String(slot.date.getHours()).padStart(2, '0'),
        slots: [],
      })
    }
    groups.get(key)?.slots.push(slot)
  }

  return [...groups.values()]
})
const selectedHourGroupIndex = computed(() => {
  if (!selectedSlot.value) return 0
  const key = `${selectedSlot.value.isoDate}-${selectedSlot.value.date.getHours()}`
  return Math.max(0, hourGroups.value.findIndex((group) => group.key === key))
})
const selectedHourGroup = computed(() => hourGroups.value[selectedHourGroupIndex.value])
const selectedTrack = computed(() => {
  const selectedId = audioSelection.value?.selected_track_id
  return musicTracks.value.find((track) => track.id === selectedId) ?? null
})
const selectedAudioLabel = computed(() => {
  if (audioSelection.value?.audio_source === 'fallback') return 'Son de secours local'
  return selectedTrack.value?.title ?? 'Aucune musique sélectionnée'
})
const configuredAlarmDate = computed(() => {
  if (!alarmPlan.value?.alarm_date || !alarmPlan.value?.alarm_time) return null
  const date = new Date(`${alarmPlan.value.alarm_date}T${alarmPlan.value.alarm_time}`)
  return Number.isNaN(date.getTime()) ? null : date
})
const alarmIsActive = computed(() => Boolean(alarmPlan.value?.enabled))
const alarmIsExpired = computed(() => {
  if (!configuredAlarmDate.value) return false
  return nowTick.value >= configuredAlarmDate.value.getTime() + ALARM_WINDOW_MS
})
const alarmIsLocked = computed(() => {
  if (!alarmIsActive.value || !configuredAlarmDate.value) return false
  const alarmTime = configuredAlarmDate.value.getTime()
  return nowTick.value >= alarmTime - ALARM_LOCK_BEFORE_MS && nowTick.value < alarmTime + ALARM_WINDOW_MS
})
const showAlarmCard = computed(() => alarmIsActive.value && Boolean(configuredAlarmDate.value) && !alarmIsExpired.value)
const alarmRemainingLabel = computed(() => {
  if (!configuredAlarmDate.value) return ''
  const delta = configuredAlarmDate.value.getTime() - nowTick.value
  if (delta <= 0) return 'En cours'
  return formatRelativeHours(delta / 3_600_000)
})
const alarmDateLabel = computed(() => {
  if (!configuredAlarmDate.value) return 'Aucune date'
  return configuredAlarmDate.value.toLocaleDateString('fr-CH', {
    weekday: 'long',
    day: 'numeric',
    month: 'long',
  })
})
const alarmUnlockDeadline = computed(() => {
  if (!configuredAlarmDate.value) return ''
  const deadline = new Date(configuredAlarmDate.value.getTime() - ALARM_LOCK_BEFORE_MS)
  return deadline.toLocaleTimeString('fr-CH', { hour: '2-digit', minute: '2-digit', hour12: false })
})
const hasProblem = computed(() => {
  return stationStatus.value?.problem_code !== 'none' || braceletStatus.value?.problem_code !== 'none'
})
const stationStale = computed(() => isStale(stationStatus.value?.updated_at))
const braceletStale = computed(() => isStale(braceletStatus.value?.updated_at))
const systemReady = computed(() => {
  return Boolean(stationStatus.value && braceletStatus.value && !hasProblem.value && !stationStale.value && !braceletStale.value)
})
const canUseSupabase = computed(() => SUPABASE_CONFIGURED)
const stationBatteryPercent = computed(() => estimateBatteryPercent(stationStatus.value?.station_battery_voltage))
const braceletBatteryPercent = computed(() => estimateBatteryPercent(braceletStatus.value?.bracelet_battery_voltage))
const deviceWarning = computed(() => {
  const bracelet = braceletStatus.value
  const station = stationStatus.value
  if (bracelet && bracelet.problem_code !== 'none') {
    return bracelet.problem_message || problemLabels[bracelet.problem_code]
  }
  if (station && station.problem_code !== 'none') {
    return station.problem_message || problemLabels[station.problem_code]
  }
  if (braceletStale.value) return 'Les données du bracelet ne sont plus à jour.'
  if (stationStale.value) return 'Les données de la station ne sont plus à jour.'
  return ''
})

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
  music_stream_failed: 'Lecture de la musique échouée',
  fallback_audio_failed: 'Son de secours indisponible',
  bracelet_missing: 'Bracelet absent',
  bracelet_low_battery: 'Batterie du bracelet bientôt faible',
  bracelet_fault: 'Défaut du bracelet',
  sensor_fault: 'Défaut du capteur',
  audio_fault: 'Défaut audio',
  unknown_fault: 'Défaut inconnu',
}

onMounted(() => {
  void loadInitialData()
  startStatusPolling()
  startRealtimeUpdates()
  document.addEventListener('visibilitychange', handleVisibilityChange)
  slotTimer = window.setInterval(refreshSlots, 60_000)
  void nextTick(syncWheels)
})

onBeforeUnmount(() => {
  document.removeEventListener('visibilitychange', handleVisibilityChange)
  stopStatusPolling()
  stopRealtimeUpdates()
  if (slotTimer) window.clearInterval(slotTimer)
  if (hourScrollTimer) window.clearTimeout(hourScrollTimer)
  if (minuteScrollTimer) window.clearTimeout(minuteScrollTimer)
  stopPreview()
})

watch([selectedSlotIndex, activeTab], () => void nextTick(syncWheels))

function isPageVisible() {
  return document.visibilityState === 'visible'
}

function startStatusPolling() {
  if (pollTimer || !isPageVisible()) return
  pollTimer = window.setInterval(() => void loadStatuses(), STATUS_POLL_MS)
}

function toggleTechnicalDetails() {
  technicalDetailsOpen.value = !technicalDetailsOpen.value
  if (technicalDetailsOpen.value) void loadStatuses()
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

  void loadInitialData()
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
  if (alarmIsLocked.value) {
    showLockedMessage()
    return
  }
  if (!selectedSlot.value || !audioSelection.value || !canUseSupabase.value) return
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
    nowTick.value = Date.now()
    successMessage.value = 'Alarme fixée.'
  } catch (error) {
    showError(error)
  } finally {
    saving.value = false
  }
}

async function disableAlarm() {
  if (alarmIsLocked.value) {
    showLockedMessage()
    return
  }
  if (!alarmPlan.value || !alarmPlan.value.enabled || !canUseSupabase.value) return
  clearMessages()
  saving.value = true

  try {
    const client = requireSupabase()
    const { data, error } = await client
      .from('alarm_plan')
      .upsert({
        ...alarmPlan.value,
        enabled: false,
        revision: (alarmPlan.value.revision ?? 1) + 1,
      })
      .select()
      .single()

    if (error) throw error
    alarmPlan.value = data as AlarmPlan
    successMessage.value = 'Alarme supprimée.'
  } catch (error) {
    showError(error)
  } finally {
    saving.value = false
  }
}

async function chooseFallback() {
  if (alarmIsLocked.value) {
    showLockedMessage()
    return
  }
  await updateAudioSelection(null)
}

async function chooseTrack(track: MusicTrack) {
  if (alarmIsLocked.value) {
    showLockedMessage()
    return
  }
  await updateAudioSelection(track.id)
  openTrackMenuId.value = null
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
  if ([bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]].some((value) => value & 0x80)) return -1
  return (bytes[offset] << 21) | (bytes[offset + 1] << 14) | (bytes[offset + 2] << 7) | bytes[offset + 3]
}

async function prepareAudioFileForUpload(file: File): Promise<PreparedAudioFile> {
  if (!isMp3File(file)) return { file, strippedBytes: 0 }

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

  if (end - start >= 128 && bytes[end - 128] === 0x54 && bytes[end - 127] === 0x41 && bytes[end - 126] === 0x47) end -= 128
  if (start === 0 && end === bytes.length) return { file, strippedBytes: 0 }
  if (start >= end) throw new Error('Le nettoyage a retiré tout le fichier MP3. Réencode le fichier avant de l’ajouter.')

  return {
    file: new File([buffer.slice(start, end)], file.name, { type: 'audio/mpeg', lastModified: Date.now() }),
    strippedBytes: bytes.length - (end - start),
  }
}

async function updateAudioSelection(trackId: string | null) {
  if (alarmIsLocked.value || !canUseSupabase.value) return
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
      client.from('alarm_plan').upsert({
        id: 'main',
        enabled: alarmPlan.value?.enabled ?? false,
        alarm_date: alarmPlan.value?.alarm_date ?? null,
        alarm_time: alarmPlan.value?.alarm_time ?? null,
        timezone: alarmPlan.value?.timezone ?? 'Europe/Zurich',
        revision: nextRevision,
      }).select().single(),
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

function openMusicPicker() {
  fileInput.value?.click()
}

async function uploadMusic(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file) return
  clearMessages()
  uploading.value = true

  try {
    if (!isAcceptedAudioFile(file)) throw new Error('Format non accepté. Utilise MP3, WAV, OGG ou MP4 audio.')
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
  if (alarmIsLocked.value && audioSelection.value?.selected_track_id === track.id) {
    showLockedMessage()
    return
  }
  clearMessages()
  stopPreview()

  try {
    const client = requireSupabase()
    const { error: storageError } = await client.storage.from('wake-up-music').remove([track.storage_path])
    if (storageError) throw storageError
    const { error: deleteError } = await client.from('music_tracks').delete().eq('id', track.id)
    if (deleteError) throw deleteError
    if (audioSelection.value?.selected_track_id === track.id) await chooseFallback()
    await loadMusicTracks()
    openTrackMenuId.value = null
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
  audioElement.value?.pause()
  audioElement.value = null
  playingTrackId.value = null
}

function refreshSlots() {
  nowTick.value = Date.now()
  const previous = selectedSlot.value
  slots.value = buildAlarmSlots()
  const newIndex = slots.value.findIndex((slot) => slot.isoDate === previous?.isoDate && slot.sqlTime === previous?.sqlTime)
  selectedSlotIndex.value = newIndex >= 0 ? newIndex : 0
}

function selectHour(group: HourGroup) {
  if (alarmIsLocked.value) return
  const preferredMinute = selectedSlot.value?.date.getMinutes() ?? 0
  const nextSlot = group.slots.reduce((closest, slot) => {
    return Math.abs(slot.date.getMinutes() - preferredMinute) < Math.abs(closest.date.getMinutes() - preferredMinute) ? slot : closest
  }, group.slots[0])
  selectSlot(nextSlot)
}

function selectSlot(slot: AlarmSlot) {
  if (alarmIsLocked.value) return
  const index = slots.value.findIndex((candidate) => candidate.isoDate === slot.isoDate && candidate.sqlTime === slot.sqlTime)
  if (index >= 0) selectedSlotIndex.value = index
}

function handleWheelScroll(kind: 'hour' | 'minute') {
  const timer = kind === 'hour' ? hourScrollTimer : minuteScrollTimer
  if (timer) window.clearTimeout(timer)
  const nextTimer = window.setTimeout(() => settleWheel(kind), 110)
  if (kind === 'hour') hourScrollTimer = nextTimer
  else minuteScrollTimer = nextTimer
}

function settleWheel(kind: 'hour' | 'minute') {
  if (alarmIsLocked.value) return
  const container = kind === 'hour' ? hourWheel.value : minuteWheel.value
  if (!container) return
  const items = [...container.querySelectorAll<HTMLElement>('[data-wheel-item]')]
  const center = container.scrollTop + container.clientHeight / 2
  const nearest = items.reduce<HTMLElement | null>((best, item) => {
    if (!best) return item
    const itemCenter = item.offsetTop + item.offsetHeight / 2
    const bestCenter = best.offsetTop + best.offsetHeight / 2
    return Math.abs(itemCenter - center) < Math.abs(bestCenter - center) ? item : best
  }, null)
  nearest?.click()
}

function syncWheels() {
  scrollSelectedIntoView(hourWheel.value)
  scrollSelectedIntoView(minuteWheel.value)
}

function scrollSelectedIntoView(container: HTMLElement | null) {
  const selected = container?.querySelector<HTMLElement>('[aria-selected="true"]')
  if (!container || !selected) return
  container.scrollTo({ top: selected.offsetTop - (container.clientHeight - selected.offsetHeight) / 2, behavior: 'smooth' })
}

function coverClass(seed?: string | null) {
  if (!seed) return 'cover-0'
  const value = [...seed].reduce((sum, character) => sum + character.charCodeAt(0), 0)
  return `cover-${value % 4}`
}

function showLockedMessage() {
  clearMessages()
  errorMessage.value = 'Cette alarme est verrouillée pendant la dernière heure et jusqu’à la fin du réveil.'
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
  errorMessage.value = error instanceof Error ? error.message : 'Erreur inconnue.'
}

function formatBatteryPercent(value?: number | null) {
  return value === null || value === undefined ? '—' : `${value}%`
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
  if (!value) return 'Inconnue'
  const date = new Date(value)
  const seconds = Math.max(0, Math.round((nowTick.value - date.getTime()) / 1000))
  if (seconds < 60) return 'À l’instant'
  if (seconds < 3600) return `Il y a ${Math.round(seconds / 60)} min`
  return date.toLocaleString('fr-CH', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' })
}
</script>

<template>
  <main class="app-shell">
    <header class="app-header">
      <div>
        <p class="app-kicker">Starter</p>
        <h1>{{ activeTab === 'alarm' ? 'Réveil' : activeTab === 'music' ? 'Musiques' : 'Appareils' }}</h1>
      </div>
      <button
        v-if="activeTab === 'music'"
        class="header-action"
        type="button"
        :disabled="uploading || !canUseSupabase"
        aria-label="Ajouter une musique"
        @click="openMusicPicker"
      >
        <Loader2 v-if="uploading" class="spin" :size="22" />
        <Plus v-else :size="24" />
      </button>
      <span v-else class="header-status" :class="{ warning: hasProblem || stationStale || braceletStale }"></span>
      <input
        ref="fileInput"
        class="visually-hidden"
        type="file"
        accept="audio/mpeg,audio/wav,audio/ogg,audio/mp4"
        :disabled="uploading"
        @change="uploadMusic"
      />
    </header>

    <div class="message-stack" aria-live="polite">
      <section v-if="!canUseSupabase" class="notice danger">
        <CloudOff :size="18" />
        <span>Supabase n’est pas configuré. Ajoute les variables dans `.env.local`.</span>
      </section>
      <section v-if="errorMessage" class="notice danger">
        <CircleAlert :size="18" />
        <span>{{ errorMessage }}</span>
      </section>
      <section v-if="successMessage" class="notice success">
        <Check :size="18" />
        <span>{{ successMessage }}</span>
      </section>
    </div>

    <section v-if="loading" class="loading-state">
      <Loader2 class="spin" :size="28" />
      <span>Préparation de votre réveil</span>
    </section>

    <template v-else>
      <section v-if="activeTab === 'alarm'" class="page alarm-page">
        <template v-if="!showAlarmCard">
          <div class="time-picker" aria-label="Choisir l’heure du réveil">
            <div class="selection-glow"></div>
            <div
              ref="hourWheel"
              class="wheel-column"
              role="listbox"
              aria-label="Heures"
              tabindex="0"
              @scroll.passive="handleWheelScroll('hour')"
            >
              <button
                v-for="group in hourGroups"
                :key="group.key"
                data-wheel-item
                type="button"
                role="option"
                :aria-selected="selectedHourGroup?.key === group.key"
                @click="selectHour(group)"
              >
                <span>{{ group.hour }}</span>
              </button>
            </div>
            <span class="time-separator">:</span>
            <div
              ref="minuteWheel"
              class="wheel-column"
              role="listbox"
              aria-label="Minutes"
              tabindex="0"
              @scroll.passive="handleWheelScroll('minute')"
            >
              <button
                v-for="slot in selectedHourGroup?.slots ?? []"
                :key="slot.sqlTime"
                data-wheel-item
                type="button"
                role="option"
                :aria-selected="selectedSlot?.sqlTime === slot.sqlTime"
                @click="selectSlot(slot)"
              >
                <span>{{ slot.shortLabel.slice(-2) }}</span>
              </button>
            </div>
          </div>

          <section class="selected-sound-card">
            <div class="cover-art large" :class="coverClass(selectedTrack?.id)">
              <Music :size="25" />
            </div>
            <button class="sound-copy" type="button" @click="activeTab = 'music'">
              <small>Son du réveil</small>
              <strong>{{ selectedAudioLabel }}</strong>
            </button>
            <button
              class="round-button"
              type="button"
              :disabled="!selectedTrack"
              :aria-label="playingTrackId === selectedTrack?.id ? 'Mettre en pause' : 'Écouter la musique'"
              @click="selectedTrack && previewTrack(selectedTrack)"
            >
              <Pause v-if="playingTrackId === selectedTrack?.id" :size="19" />
              <Play v-else :size="19" />
            </button>
            <ChevronRight class="sound-chevron" :size="19" />
          </section>

          <label class="volume-card">
            <span><Volume2 :size="18" /> Volume</span>
            <strong>{{ draftVolume }} %</strong>
            <input v-model.number="draftVolume" type="range" min="0" max="100" step="5" />
          </label>

          <button
            class="primary-action"
            type="button"
            :disabled="saving || !canUseSupabase || !audioSelection"
            @click="saveAlarm"
          >
            <Loader2 v-if="saving" class="spin" :size="19" />
            <Bell v-else :size="19" />
            Fixer l’alarme
          </button>
        </template>

        <section v-if="showAlarmCard" class="fixed-alarm-card" :class="{ locked: alarmIsLocked }">
          <div class="alarm-card-topline">
            <span><i></i> Alarme fixée</span>
            <span>{{ alarmRemainingLabel }}</span>
          </div>
          <div class="alarm-card-main">
            <div>
              <strong class="fixed-time">{{ configuredAlarmDate?.toLocaleTimeString('fr-CH', { hour: '2-digit', minute: '2-digit', hour12: false }) }}</strong>
              <p>{{ alarmDateLabel }}</p>
            </div>
            <button
              class="delete-alarm"
              type="button"
              :class="{ locked: alarmIsLocked }"
              :disabled="saving || alarmIsLocked"
              :aria-label="alarmIsLocked ? 'Suppression verrouillée' : 'Supprimer l’alarme'"
              @click="disableAlarm"
            >
              <LockKeyhole v-if="alarmIsLocked" :size="19" />
              <X v-else :size="23" />
            </button>
          </div>
          <div class="alarm-sound-summary">
            <div class="cover-art small" :class="coverClass(selectedTrack?.id)"><Music :size="15" /></div>
            <span>{{ selectedAudioLabel }} · {{ draftVolume }} %</span>
          </div>
          <p class="lock-caption">
            {{ alarmIsLocked ? 'Suppression et réglages verrouillés' : `Modifiable jusqu’à ${alarmUnlockDeadline}` }}
          </p>
        </section>

        <div v-else class="empty-alarm"><span></span> Aucune alarme fixée</div>
      </section>

      <section v-else-if="activeTab === 'music'" class="page music-page">
        <section v-if="selectedTrack" class="featured-track">
          <div class="cover-art featured" :class="coverClass(selectedTrack?.id)"><Music :size="34" /></div>
          <div class="featured-copy">
            <small>Son sélectionné</small>
            <strong>{{ selectedAudioLabel }}</strong>
            <span>{{ alarmIsLocked ? 'Verrouillé pour le réveil en cours' : 'Utilisé pour le prochain réveil' }}</span>
          </div>
          <button
            class="round-button prominent"
            type="button"
            :disabled="!selectedTrack"
            @click="selectedTrack && previewTrack(selectedTrack)"
          >
            <Pause v-if="playingTrackId === selectedTrack?.id" :size="20" />
            <Play v-else :size="20" />
          </button>
          <Check class="selected-check" :size="18" />
        </section>

        <div class="section-heading">
          <h2>Bibliothèque</h2>
          <span>{{ musicTracks.length }} son{{ musicTracks.length > 1 ? 's' : '' }}</span>
        </div>

        <div class="track-list">
          <article v-for="track in musicTracks" :key="track.id" class="track-row" :class="{ selected: selectedTrack?.id === track.id }">
            <button class="track-select" type="button" :disabled="alarmIsLocked" @click="chooseTrack(track)">
              <div class="cover-art track-cover" :class="coverClass(track.id)"><Music :size="18" /></div>
              <span class="track-copy">
                <strong>{{ track.title }}</strong>
                <small>{{ track.is_available ? 'Disponible' : 'Indisponible' }}</small>
              </span>
              <Check v-if="selectedTrack?.id === track.id" class="row-check" :size="19" />
            </button>
            <button class="round-button compact" type="button" :aria-label="`Écouter ${track.title}`" @click="previewTrack(track)">
              <Pause v-if="playingTrackId === track.id" :size="16" />
              <Play v-else :size="16" />
            </button>
            <div class="track-menu-wrap">
              <button class="more-button" type="button" :aria-label="`Actions pour ${track.title}`" @click="openTrackMenuId = openTrackMenuId === track.id ? null : track.id">
                <Ellipsis :size="20" />
              </button>
              <div v-if="openTrackMenuId === track.id" class="track-menu">
                <button type="button" :disabled="alarmIsLocked && selectedTrack?.id === track.id" @click="deleteTrack(track)">
                  <Trash2 :size="16" /> Supprimer
                </button>
              </div>
            </div>
          </article>
        </div>

        <div v-if="musicTracks.length === 0" class="empty-library">
          <Music :size="27" />
          <strong>Aucune musique</strong>
          <span>Utilise le bouton + pour ajouter ton premier son.</span>
        </div>
      </section>

      <section v-else class="page devices-page">
        <section class="readiness" :class="{ warning: !systemReady }">
          <div class="readiness-icon">
            <ShieldCheck v-if="systemReady" :size="24" />
            <AlertTriangle v-else :size="24" />
          </div>
          <div>
            <small>État général</small>
            <strong>{{ systemReady ? 'Tout est prêt' : 'Attention requise' }}</strong>
          </div>
          <span class="live-dot" :class="{ warning: !systemReady }"></span>
        </section>

        <div class="device-list">
          <article class="device-row">
            <div class="device-icon"><Radio :size="22" /></div>
            <div class="device-main">
              <strong>Station</strong>
              <span>{{ stationStatus ? stationStateLabels[stationStatus.station_state] : 'Inconnue' }}</span>
            </div>
            <div class="device-meta">
              <strong>{{ formatBatteryPercent(stationBatteryPercent) }}</strong>
              <span>{{ formatUpdatedAt(stationStatus?.updated_at) }}</span>
            </div>
            <span class="live-dot" :class="{ warning: stationStale || stationStatus?.problem_code !== 'none' }"></span>
          </article>

          <article class="device-row">
            <div class="device-icon">
              <BatteryCharging v-if="braceletStatus?.bracelet_state === 'charging'" :size="22" />
              <Watch v-else :size="22" />
            </div>
            <div class="device-main">
              <strong>Bracelet</strong>
              <span>{{ braceletStatus ? braceletStateLabels[braceletStatus.bracelet_state] : 'Inconnu' }}</span>
            </div>
            <div class="device-meta">
              <strong>{{ formatBatteryPercent(braceletBatteryPercent) }}</strong>
              <span>{{ formatUpdatedAt(braceletStatus?.updated_at) }}</span>
            </div>
            <span class="live-dot" :class="{ warning: braceletStale || braceletStatus?.problem_code !== 'none' }"></span>
          </article>
        </div>

        <section v-if="deviceWarning" class="device-warning">
          <AlertTriangle :size="19" />
          <div><small>À vérifier</small><strong>{{ deviceWarning }}</strong></div>
        </section>

        <section class="technical-panel">
          <button type="button" @click="toggleTechnicalDetails">
            <span>Détails techniques</span>
            <ChevronDown :class="{ open: technicalDetailsOpen }" :size="20" />
          </button>
          <div v-if="technicalDetailsOpen" class="technical-content">
            <div>
              <span>Station</span>
              <strong>{{ stationStatus ? problemLabels[stationStatus.problem_code] : 'Aucune donnée' }}</strong>
              <small>{{ stationStatus?.problem_message || 'Pas de message de diagnostic' }}</small>
              <small>Batterie : {{ formatBatteryVoltage(stationStatus?.station_battery_voltage) }}</small>
            </div>
            <div>
              <span>Bracelet</span>
              <strong>{{ braceletStatus ? problemLabels[braceletStatus.problem_code] : 'Aucune donnée' }}</strong>
              <small>{{ braceletStatus?.problem_message || 'Pas de message de diagnostic' }}</small>
              <small>Batterie : {{ formatBatteryVoltage(braceletStatus?.bracelet_battery_voltage) }}</small>
            </div>
          </div>
        </section>
      </section>
    </template>

    <nav class="bottom-nav" aria-label="Navigation principale">
      <button type="button" :class="{ active: activeTab === 'alarm' }" @click="activeTab = 'alarm'">
        <Bell :size="21" /><span>Réveil</span>
      </button>
      <button type="button" :class="{ active: activeTab === 'music' }" @click="activeTab = 'music'">
        <Music :size="21" /><span>Musiques</span>
      </button>
      <button type="button" :class="{ active: activeTab === 'devices' }" @click="activeTab = 'devices'">
        <Watch :size="21" /><span>Appareils</span>
      </button>
    </nav>
  </main>
</template>
