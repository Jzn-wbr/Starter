export interface AlarmSlot {
  date: Date
  label: string
  shortLabel: string
  isoDate: string
  sqlTime: string
  hoursFromNow: number
}

const WINDOW_HOURS = 12
const SLOT_MINUTES = 5

export function buildAlarmSlots(now = new Date()): AlarmSlot[] {
  const first = new Date(now)
  const roundedMinutes = Math.ceil(first.getMinutes() / SLOT_MINUTES) * SLOT_MINUTES
  first.setMinutes(roundedMinutes, 0, 0)

  if (first <= now) {
    first.setMinutes(first.getMinutes() + SLOT_MINUTES)
  }

  const limit = new Date(now.getTime() + WINDOW_HOURS * 60 * 60 * 1000)
  const slots: AlarmSlot[] = []

  for (const cursor = new Date(first); cursor <= limit; cursor.setMinutes(cursor.getMinutes() + SLOT_MINUTES)) {
    const date = new Date(cursor)
    const hoursFromNow = (date.getTime() - now.getTime()) / 3_600_000
    slots.push({
      date,
      label: formatSlotLabel(date, now),
      shortLabel: formatTime(date),
      isoDate: toLocalDate(date),
      sqlTime: `${formatTime(date)}:00`,
      hoursFromNow,
    })
  }

  return slots
}

export function pickInitialSlot(slots: AlarmSlot[], alarmDate: string | null, alarmTime: string | null) {
  if (!alarmDate || !alarmTime) return 0

  const current = slots.findIndex((slot) => slot.isoDate === alarmDate && slot.sqlTime.startsWith(alarmTime.slice(0, 5)))
  return current >= 0 ? current : 0
}

export function formatTime(date: Date) {
  return new Intl.DateTimeFormat('fr-CH', {
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  }).format(date)
}

export function formatRelativeHours(hours: number) {
  if (hours < 1) return 'dans moins d’une heure'
  const rounded = Math.round(hours * 10) / 10
  return `dans ${rounded.toLocaleString('fr-CH')} h`
}

function formatSlotLabel(date: Date, now: Date) {
  const day = date.getDate() === now.getDate() ? 'Aujourd’hui' : 'Demain'
  return `${day} ${formatTime(date)}`
}

function toLocalDate(date: Date) {
  const year = date.getFullYear()
  const month = String(date.getMonth() + 1).padStart(2, '0')
  const day = String(date.getDate()).padStart(2, '0')
  return `${year}-${month}-${day}`
}
