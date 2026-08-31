// feeds-backend.h — Single source of truth for the Feeds entitlement backend
// (the Cloudflare Worker) hostname.
//
// The engine talks to this Worker at two endpoints:
//   GET /tier                 — the account's entitlement tier (engine-api.cpp)
//   GET /authresult?state=... — the OAuth code handoff  (engine-oauth.cpp)
//
// Both narrow and wide forms are needed — WinHTTP takes wide strings, logging
// and JSON take narrow — so the host is a macro and every form below is built
// from it by string-literal concatenation. Changing the domain is a one-line
// edit here; nothing else in this repo spells the hostname out.
//
// OUTSIDE THIS REPO: the loginsuccess page on letsdovideo.com POSTs the auth
// code to this same Worker. That reference lives on the website, not in the
// code, so a domain migration must update it there too.

#pragma once

#define FEEDS_BACKEND_HOST "feeds-entitlement.square-dust-0e00.workers.dev"

// Widen a macro's string literal. Two levels so the argument expands before
// the L is pasted on.
#define FEEDS_WIDEN_INNER(x) L##x
#define FEEDS_WIDEN(x)       FEEDS_WIDEN_INNER(x)

#define FEEDS_BACKEND_HOST_W   FEEDS_WIDEN(FEEDS_BACKEND_HOST)
#define FEEDS_BACKEND_ORIGIN   "https://" FEEDS_BACKEND_HOST
#define FEEDS_BACKEND_ORIGIN_W L"https://" FEEDS_BACKEND_HOST_W
