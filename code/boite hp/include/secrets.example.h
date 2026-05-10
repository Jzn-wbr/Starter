#pragma once

static const char *WIFI_SSID = "TON_WIFI";
static const char *WIFI_PASSWORD = "TON_MOT_DE_PASSE";

// Prototype-only Supabase anon configuration.
// Copy this file to include/secrets.h and fill the values locally.
static const char *SUPABASE_URL = "https://TON_PROJET.supabase.co";
static const char *SUPABASE_ANON_KEY = "TON_ANON_KEY";

// Keep true for the real product. Set false only for bench audio tests without the bracelet.
static const bool REQUIRE_BRACELET_READY = true;
