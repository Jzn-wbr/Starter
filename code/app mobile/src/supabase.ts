import { createClient } from '@supabase/supabase-js'

export type AudioSource = 'track' | 'fallback'
export type StationState = 'idle' | 'armed' | 'ringing' | 'validating_activity' | 'stopped' | 'fault'
export type BraceletState = 'unknown' | 'charging' | 'ready' | 'active' | 'validated' | 'low_battery' | 'fault'
export type ProblemCode =
  | 'none'
  | 'wifi_unavailable'
  | 'supabase_unavailable'
  | 'time_unknown'
  | 'no_valid_alarm_config'
  | 'music_stream_failed'
  | 'fallback_audio_failed'
  | 'bracelet_missing'
  | 'bracelet_low_battery'
  | 'bracelet_fault'
  | 'sensor_fault'
  | 'audio_fault'
  | 'unknown_fault'

export interface MusicTrack {
  id: string
  title: string
  storage_path: string
  public_url: string
  is_available: boolean
  created_at: string
}

export interface AlarmPlan {
  id: 'main'
  enabled: boolean
  alarm_date: string | null
  alarm_time: string | null
  timezone: string
  revision: number
  updated_at: string
}

export interface AlarmAudioSelection {
  id: 'main'
  audio_source: AudioSource
  selected_track_id: string | null
  volume_percent: number
  updated_at: string
}

export interface StationStatus {
  id: 'main'
  station_state: StationState
  problem_code: ProblemCode
  problem_message: string
  active_alarm_revision: number | null
  station_battery_voltage: number | null
  updated_at: string
}

export interface BraceletStatus {
  id: 'main'
  bracelet_state: BraceletState
  problem_code: ProblemCode
  problem_message: string
  bracelet_battery_voltage: number | null
  bracelet_last_seen_ms: number | null
  updated_at: string
}

export const SUPABASE_CONFIGURED =
  Boolean(import.meta.env.VITE_SUPABASE_URL) && Boolean(import.meta.env.VITE_SUPABASE_ANON_KEY)

export const supabase = SUPABASE_CONFIGURED
  ? createClient(import.meta.env.VITE_SUPABASE_URL, import.meta.env.VITE_SUPABASE_ANON_KEY)
  : null

export function requireSupabase() {
  if (!supabase) {
    throw new Error('Supabase non configuré. Crée un fichier .env.local depuis .env.example.')
  }

  return supabase
}
