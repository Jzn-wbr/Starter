-- Supabase v1 schema for the personal wake-up prototype.
-- Prototype security model: anon clients may read and write the v1 tables and
-- wake-up music bucket. This is not production-ready access control.

create extension if not exists pgcrypto;

do $$
begin
  create type audio_source_v1 as enum (
    'track',
    'fallback'
  );
exception
  when duplicate_object then null;
end $$;

do $$
begin
  create type station_state_v1 as enum (
    'idle',
    'armed',
    'ringing',
    'validating_activity',
    'stopped',
    'fault'
  );
exception
  when duplicate_object then null;
end $$;

do $$
begin
  create type bracelet_state_v1 as enum (
    'unknown',
    'charging',
    'ready',
    'active',
    'validated',
    'low_battery',
    'fault'
  );
exception
  when duplicate_object then null;
end $$;

do $$
begin
  create type problem_code_v1 as enum (
    'none',
    'wifi_unavailable',
    'supabase_unavailable',
    'time_unknown',
    'no_valid_alarm_config',
    'music_stream_failed',
    'fallback_audio_failed',
    'bracelet_missing',
    'bracelet_low_battery',
    'bracelet_fault',
    'sensor_fault',
    'audio_fault',
    'unknown_fault'
  );
exception
  when duplicate_object then null;
end $$;

create or replace function set_updated_at_v1()
returns trigger
language plpgsql
as $$
begin
  new.updated_at = now();
  return new;
end;
$$;

create table if not exists music_tracks (
  id text primary key default gen_random_uuid()::text,
  title text not null check (length(trim(title)) > 0),
  storage_path text not null unique check (length(trim(storage_path)) > 0),
  public_url text not null check (length(trim(public_url)) > 0),
  is_available boolean not null default true,
  created_at timestamptz not null default now()
);

create table if not exists alarm_config (
  id text primary key default 'main',
  enabled boolean not null default false,
  alarm_date date,
  alarm_time time without time zone,
  timezone text not null default 'Europe/Zurich',
  volume_percent integer not null default 70 check (volume_percent between 0 and 100),
  audio_source audio_source_v1 not null default 'track',
  selected_track_id text references music_tracks(id) on update cascade on delete set null,
  revision integer not null default 1 check (revision >= 1),
  updated_at timestamptz not null default now(),
  constraint alarm_config_single_row check (id = 'main'),
  constraint alarm_config_enabled_requires_time check (
    enabled = false
    or (alarm_date is not null and alarm_time is not null)
  ),
  constraint alarm_config_enabled_track_requires_selection check (
    enabled = false
    or audio_source = 'fallback'
    or selected_track_id is not null
  ),
  constraint alarm_config_fallback_has_no_selected_track check (
    audio_source <> 'fallback'
    or selected_track_id is null
  )
);

create table if not exists device_status (
  id text primary key default 'main',
  station_state station_state_v1 not null default 'idle',
  bracelet_state bracelet_state_v1 not null default 'unknown',
  problem_code problem_code_v1 not null default 'none',
  problem_message text not null default '',
  active_alarm_revision integer check (
    active_alarm_revision is null or active_alarm_revision >= 1
  ),
  bracelet_battery_percent integer check (
    bracelet_battery_percent is null
    or bracelet_battery_percent between 0 and 100
  ),
  bracelet_last_seen_ms bigint check (
    bracelet_last_seen_ms is null or bracelet_last_seen_ms >= 0
  ),
  updated_at timestamptz not null default now(),
  constraint device_status_single_row check (id = 'main')
);

drop trigger if exists alarm_config_set_updated_at on alarm_config;
create trigger alarm_config_set_updated_at
before update on alarm_config
for each row
execute function set_updated_at_v1();

drop trigger if exists device_status_set_updated_at on device_status;
create trigger device_status_set_updated_at
before update on device_status
for each row
execute function set_updated_at_v1();

insert into alarm_config (id, enabled, timezone, revision)
values ('main', false, 'Europe/Zurich', 1)
on conflict (id) do nothing;

insert into device_status (id, station_state, bracelet_state, problem_code)
values ('main', 'idle', 'unknown', 'none')
on conflict (id) do nothing;

insert into storage.buckets (
  id,
  name,
  public,
  file_size_limit,
  allowed_mime_types
)
values (
  'wake-up-music',
  'wake-up-music',
  true,
  52428800,
  array['audio/mpeg', 'audio/wav', 'audio/ogg', 'audio/mp4']
)
on conflict (id) do update set
  public = excluded.public,
  file_size_limit = excluded.file_size_limit,
  allowed_mime_types = excluded.allowed_mime_types;

alter table music_tracks enable row level security;
alter table alarm_config enable row level security;
alter table device_status enable row level security;

grant usage on schema public to anon;
grant select, insert, update, delete on music_tracks to anon;
grant select, insert, update, delete on alarm_config to anon;
grant select, insert, update, delete on device_status to anon;

drop policy if exists prototype_music_tracks_all on music_tracks;
create policy prototype_music_tracks_all
on music_tracks
for all
to anon
using (true)
with check (true);

drop policy if exists prototype_alarm_config_all on alarm_config;
create policy prototype_alarm_config_all
on alarm_config
for all
to anon
using (true)
with check (id = 'main');

drop policy if exists prototype_device_status_all on device_status;
create policy prototype_device_status_all
on device_status
for all
to anon
using (true)
with check (id = 'main');

drop policy if exists prototype_wake_up_music_select on storage.objects;
create policy prototype_wake_up_music_select
on storage.objects
for select
to anon
using (bucket_id = 'wake-up-music');

drop policy if exists prototype_wake_up_music_insert on storage.objects;
create policy prototype_wake_up_music_insert
on storage.objects
for insert
to anon
with check (bucket_id = 'wake-up-music');

drop policy if exists prototype_wake_up_music_update on storage.objects;
create policy prototype_wake_up_music_update
on storage.objects
for update
to anon
using (bucket_id = 'wake-up-music')
with check (bucket_id = 'wake-up-music');

drop policy if exists prototype_wake_up_music_delete on storage.objects;
create policy prototype_wake_up_music_delete
on storage.objects
for delete
to anon
using (bucket_id = 'wake-up-music');
