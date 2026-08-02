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

create table if not exists alarm_plan (
  id text primary key default 'main',
  enabled boolean not null default false,
  alarm_date date,
  alarm_time time without time zone,
  timezone text not null default 'Europe/Zurich',
  revision integer not null default 1 check (revision >= 1),
  updated_at timestamptz not null default now(),
  constraint alarm_plan_single_row check (id = 'main'),
  constraint alarm_plan_enabled_requires_time check (
    enabled = false
    or (alarm_date is not null and alarm_time is not null)
  )
);

create table if not exists alarm_audio_selection (
  id text primary key default 'main',
  audio_source audio_source_v1 not null default 'track',
  selected_track_id text references music_tracks(id) on update cascade on delete set null,
  volume_percent integer not null default 70 check (volume_percent between 0 and 100),
  updated_at timestamptz not null default now(),
  constraint alarm_audio_selection_single_row check (id = 'main'),
  constraint alarm_audio_selection_track_requires_selection check (
    audio_source = 'fallback'
    or selected_track_id is not null
  ),
  constraint alarm_audio_selection_fallback_has_no_selected_track check (
    audio_source <> 'fallback'
    or selected_track_id is null
  )
);

create table if not exists station_status (
  id text primary key default 'main',
  station_state station_state_v1 not null default 'idle',
  problem_code problem_code_v1 not null default 'none',
  problem_message text not null default '',
  active_alarm_revision integer check (
    active_alarm_revision is null or active_alarm_revision >= 1
  ),
  station_battery_voltage numeric(5,3),
  updated_at timestamptz not null default now(),
  constraint station_status_single_row check (id = 'main')
);

create table if not exists bracelet_status (
  id text primary key default 'main',
  bracelet_state bracelet_state_v1 not null default 'unknown',
  problem_code problem_code_v1 not null default 'none',
  problem_message text not null default '',
  bracelet_battery_voltage numeric(5,3),
  bracelet_last_seen_ms bigint check (
    bracelet_last_seen_ms is null or bracelet_last_seen_ms >= 0
  ),
  bracelet_energy integer not null default 0 check (bracelet_energy >= 0),
  bracelet_energy_valid_ms integer not null default 0 check (
    bracelet_energy_valid_ms between 0 and 200
  ),
  bracelet_energy_threshold integer not null default 1333333 check (
    bracelet_energy_threshold >= 0
  ),
  energy_mute_remaining_ms integer not null default 0 check (
    energy_mute_remaining_ms >= 0
  ),
  bracelet_vibrating boolean not null default false,
  updated_at timestamptz not null default now(),
  constraint bracelet_status_single_row check (id = 'main')
);

alter table station_status
  add column if not exists station_battery_voltage numeric(5,3);

alter table station_status
  drop constraint if exists station_status_battery_voltage_range;

alter table station_status
  add constraint station_status_battery_voltage_range check (
    station_battery_voltage is null
    or station_battery_voltage between 0 and 6
  );

alter table bracelet_status
  add column if not exists bracelet_battery_voltage numeric(5,3);

alter table bracelet_status
  drop column if exists bracelet_battery_percent;

alter table bracelet_status
  add column if not exists bracelet_energy integer not null default 0;

alter table bracelet_status
  add column if not exists bracelet_energy_valid_ms integer not null default 0;

alter table bracelet_status
  add column if not exists bracelet_energy_threshold integer not null default 1333333;

alter table bracelet_status
  alter column bracelet_energy_threshold set default 1333333;

alter table bracelet_status
  add column if not exists energy_mute_remaining_ms integer not null default 0;

alter table bracelet_status
  add column if not exists bracelet_vibrating boolean not null default false;

alter table bracelet_status
  drop constraint if exists bracelet_status_battery_voltage_range;

alter table bracelet_status
  add constraint bracelet_status_battery_voltage_range check (
    bracelet_battery_voltage is null
    or bracelet_battery_voltage between 0 and 6
  );

alter table bracelet_status
  drop constraint if exists bracelet_status_energy_range;

alter table bracelet_status
  add constraint bracelet_status_energy_range check (bracelet_energy >= 0);

alter table bracelet_status
  drop constraint if exists bracelet_status_energy_valid_ms_range;

alter table bracelet_status
  add constraint bracelet_status_energy_valid_ms_range check (
    bracelet_energy_valid_ms between 0 and 200
  );

alter table bracelet_status
  drop constraint if exists bracelet_status_energy_threshold_range;

alter table bracelet_status
  add constraint bracelet_status_energy_threshold_range check (
    bracelet_energy_threshold >= 0
  );

alter table bracelet_status
  drop constraint if exists bracelet_status_mute_remaining_range;

alter table bracelet_status
  add constraint bracelet_status_mute_remaining_range check (
    energy_mute_remaining_ms >= 0
  );

drop trigger if exists alarm_plan_set_updated_at on alarm_plan;
create trigger alarm_plan_set_updated_at
before update on alarm_plan
for each row
execute function set_updated_at_v1();

drop trigger if exists alarm_audio_selection_set_updated_at on alarm_audio_selection;
create trigger alarm_audio_selection_set_updated_at
before update on alarm_audio_selection
for each row
execute function set_updated_at_v1();

drop trigger if exists station_status_set_updated_at on station_status;
create trigger station_status_set_updated_at
before update on station_status
for each row
execute function set_updated_at_v1();

drop trigger if exists bracelet_status_set_updated_at on bracelet_status;
create trigger bracelet_status_set_updated_at
before update on bracelet_status
for each row
execute function set_updated_at_v1();

insert into alarm_plan (id, enabled, timezone, revision)
values ('main', false, 'Europe/Zurich', 1)
on conflict (id) do nothing;

insert into alarm_audio_selection (id, audio_source, volume_percent)
values ('main', 'fallback', 70)
on conflict (id) do nothing;

insert into station_status (id, station_state, problem_code)
values ('main', 'idle', 'none')
on conflict (id) do nothing;

insert into bracelet_status (id, bracelet_state, problem_code)
values ('main', 'unknown', 'none')
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
alter table alarm_plan enable row level security;
alter table alarm_audio_selection enable row level security;
alter table station_status enable row level security;
alter table bracelet_status enable row level security;

grant usage on schema public to anon;
grant select, insert, update, delete on music_tracks to anon;
grant select, insert, update, delete on alarm_plan to anon;
grant select, insert, update, delete on alarm_audio_selection to anon;
grant select, insert, update, delete on station_status to anon;
grant select, insert, update, delete on bracelet_status to anon;

drop policy if exists prototype_music_tracks_all on music_tracks;
create policy prototype_music_tracks_all
on music_tracks
for all
to anon
using (true)
with check (true);

drop policy if exists prototype_alarm_plan_all on alarm_plan;
create policy prototype_alarm_plan_all
on alarm_plan
for all
to anon
using (true)
with check (id = 'main');

drop policy if exists prototype_alarm_audio_selection_all on alarm_audio_selection;
create policy prototype_alarm_audio_selection_all
on alarm_audio_selection
for all
to anon
using (true)
with check (id = 'main');

drop policy if exists prototype_station_status_all on station_status;
create policy prototype_station_status_all
on station_status
for all
to anon
using (true)
with check (id = 'main');

drop policy if exists prototype_bracelet_status_all on bracelet_status;
create policy prototype_bracelet_status_all
on bracelet_status
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

-- Prototype realtime feed for the PWA. Supabase projects normally provide the
-- supabase_realtime publication; this block is safe to re-run.
do $$
begin
  if exists (select 1 from pg_publication where pubname = 'supabase_realtime') then
    if not exists (
      select 1
      from pg_publication_rel pr
      join pg_class c on c.oid = pr.prrelid
      join pg_namespace n on n.oid = c.relnamespace
      where pr.prpubid = (select oid from pg_publication where pubname = 'supabase_realtime')
        and n.nspname = 'public'
        and c.relname = 'alarm_plan'
    ) then
      alter publication supabase_realtime add table alarm_plan;
    end if;

    if not exists (
      select 1
      from pg_publication_rel pr
      join pg_class c on c.oid = pr.prrelid
      join pg_namespace n on n.oid = c.relnamespace
      where pr.prpubid = (select oid from pg_publication where pubname = 'supabase_realtime')
        and n.nspname = 'public'
        and c.relname = 'alarm_audio_selection'
    ) then
      alter publication supabase_realtime add table alarm_audio_selection;
    end if;

    if not exists (
      select 1
      from pg_publication_rel pr
      join pg_class c on c.oid = pr.prrelid
      join pg_namespace n on n.oid = c.relnamespace
      where pr.prpubid = (select oid from pg_publication where pubname = 'supabase_realtime')
        and n.nspname = 'public'
        and c.relname = 'music_tracks'
    ) then
      alter publication supabase_realtime add table music_tracks;
    end if;

    if not exists (
      select 1
      from pg_publication_rel pr
      join pg_class c on c.oid = pr.prrelid
      join pg_namespace n on n.oid = c.relnamespace
      where pr.prpubid = (select oid from pg_publication where pubname = 'supabase_realtime')
        and n.nspname = 'public'
        and c.relname = 'station_status'
    ) then
      alter publication supabase_realtime add table station_status;
    end if;

    if not exists (
      select 1
      from pg_publication_rel pr
      join pg_class c on c.oid = pr.prrelid
      join pg_namespace n on n.oid = c.relnamespace
      where pr.prpubid = (select oid from pg_publication where pubname = 'supabase_realtime')
        and n.nspname = 'public'
        and c.relname = 'bracelet_status'
    ) then
      alter publication supabase_realtime add table bracelet_status;
    end if;
  end if;
end $$;
