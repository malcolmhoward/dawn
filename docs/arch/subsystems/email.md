# Email Subsystem

Source: `src/tools/email_*.c`, `src/webui/webui_email.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Multi-account email via IMAP/SMTP and Gmail REST API, with voice-controlled send, read, search, trash, and archive.

## Architecture: Dual Backend + Service Router

```
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL INTERFACE                                │
│  email_tool.c                                                        │
│  Actions: recent | read | search | folders | send | confirm_send     │
│           | accounts | trash | confirm_trash | archive               │
│  → TOOL_CAP_DANGEROUS: compile-time + runtime gates                  │
├───────────────────────────────────────────────────────────────────────┤
│                     SERVICE LAYER                                     │
│  email_service.c                                                     │
│  → Multi-account routing (dispatches to correct backend per account) │
│  → Two-step confirmation for send and trash (draft → confirm)        │
│  → Per-account read-only flag                                        │
│  → Pagination for large result sets                                  │
├───────────────────────────────────────────────────────────────────────┤
│              BACKEND A                     BACKEND B                  │
│  email_client.c (IMAP/SMTP)    gmail_client.c (Gmail REST API)      │
│  → libcurl for IMAP/SMTP       → OAuth Bearer + XOAUTH2             │
│  → App password or XOAUTH2     → REST endpoints for all operations   │
│  → Any IMAP provider           → Google-specific (thread model)      │
├───────────────────────────────────────────────────────────────────────┤
│                     SQLITE STORAGE                                    │
│  email_db.c                                                          │
│  Tables: email_accounts (encrypted passwords via crypto_store)       │
│  → Shares auth_db SQLite handle                                      │
└───────────────────────────────────────────────────────────────────────┘
```

## Key Design Points

- **Dual backend**: IMAP/SMTP for any provider, Gmail REST API for OAuth accounts (auto-selected per account).
- **Two-step confirmation**: send and trash require a confirm step — the LLM drafts, then the user confirms.
- **Contacts integration**: recipient resolution via `contacts_find()` — "email Bob" resolves to Bob's stored email.
- **Compile-time gate**: `DAWN_ENABLE_EMAIL_TOOL=ON` in CMake; runtime gate in `[email] enabled`.
- **WebUI management**: account CRUD via `webui_email.c`, Google OAuth connect flow.
