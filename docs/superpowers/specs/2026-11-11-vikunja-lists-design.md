# Vikunja lists: design

## Intent

The user wants to take the X4 to the grocery store and shop from a list
kept in their Vikunja instance. The device shows a list's open items and
lets them tick items off. It must work in the store without a network:
lists are loaded while online, ticks are saved on the SD card, and they are
sent to Vikunja the next time the device is online.

## Goals

- A **Lists** row on the Home menu, directly above **More**.
- A project list (Vikunja projects) and a task list (one project's open
  items).
- Ticking an item marks it done in Vikunja. Ticked items disappear from the
  list. The last row, "Completed", opens the done items, where ticking
  one marks it not done again.
- Works offline from the saved copy. Ticks made offline are sent later.
- A favourite list: the one starred in Vikunja, or one picked in
  Settings. Lists opens straight into it.
- Reaches a Vikunja server behind Cloudflare Access, using the same
  service-token headers that OPDS servers already use.

## Non-goals

- Adding, editing, reordering or deleting items.
- Due dates, labels, priorities, descriptions, attachments, comments.
- Hotspot handoff. The user will build that separately; this feature just
  syncs whenever the device is online.
- The Cover Grid theme. Its home tab bar stays as it is.

## Anti-goals

- Losing a tick. A tick made in the store must survive a reboot, a flat
  battery or a failed sync, and be sent eventually.
- Overwriting someone else's edits. Sync only changes an item's done state,
  nothing else.
- Credentials in plain text on the SD card after the first load.

## Constraints

- PSRAM boards (the user's X4 Pro) are the target. Response and list
  buffers go in PSRAM, following `.agents/rules/hardware-resources.md`.
- HTTPS through the existing transport (`src/network/HttpDownloader.h`).
  The SDK's wolfSSL transport has no CA bundle, so it calls `setInsecure()`
  (`SecureHttpClient.h`): traffic is encrypted, but the server certificate
  is not checked. This is the same as the OPDS and KOReader clients.
- User-facing strings go through `lib/I18n/translations/english.yaml`.

## Design

### Config file

`/vikunja.json` on the SD card (*my call*: the root, like
`/homeassistant.json`). Absent or invalid: the feature is off and the Home
menu has no Lists row. An invalid file logs the reason.

```json
{
  "url": "https://todo.example.com",
  "token": "<Vikunja API token>",
  "headers": [
    { "name": "CF-Access-Client-Id", "value": "<id>" },
    { "name": "CF-Access-Client-Secret", "value": "<secret>" }
  ]
}
```

- `url`: the base URL. `https://` or `http://`.
- `token`: a Vikunja API token, sent as `Authorization: Bearer`. It needs
  read access to projects and tasks, and update access to tasks.
- `headers`: optional extra headers, at most
  `OpdsServer::MAX_CUSTOM_HEADERS` (2), sent with every request. They use
  `HttpHeader` (`lib/HttpHeader/HttpHeader.h`) as OPDS does.
- **Protection** (the user asked for this to match the existing stores):
  on the first load, `token` and each header `value` are obfuscated with
  `obfuscation::obfuscateToBase64()`, as `src/OpdsServerStore.cpp:16-21`
  and the Home Assistant config do. They are saved as `token_obf` and
  `value_obf`, and the file is rewritten without the plain values. A new
  plain field replaces the stored one on the next load. A `_obf` value that
  does not decode counts as missing.

### Server setup (the user's side)

The server's Cloudflare Access application must accept a service token:
it needs a Service Auth policy for its own service token. Without it, a
request is redirected to the Access login (302).

### Screens

- **Home**: a Lists row above More, shown only when the config is valid.
  It uses a list or checklist icon added the same way as the More icon.
  Selecting it opens the favourite (see Settings) if it is in the saved
  copy. Otherwise it opens the project list.
- **Project list**: Vikunja projects by title, archived ones hidden,
  sub-projects indented under their parent, then a "Sync" row. Confirm
  opens a project. Back goes Home with Lists selected.
- **Status**: the header's right side shows "Syncing..." while syncing,
  else the last problem ("Offline", "Sync failed", "Token rejected",
  "Access refused", "List incomplete") and the number of ticks not yet
  sent, joined as "Sync failed, 2 not sent". Empty when all is well.
- **Task list**: the header shows the project title and the status. Rows
  are the open items in Vikunja's order, up to two lines each, or "Nothing
  left". Confirm or a tap ticks the selected item. The last row is
  "Completed" (no count: done items load a page at a time, so a count
  would only cover what is loaded). Back goes to the project list, also
  when the list was opened as the favourite.
- **Completed list**: the header reads "<list>: Completed". Done items,
  most recently done first (*my call*): ticks not yet sent, then the
  newest page from the server, with a "Load more" row while that page was
  full (at the server's real page size) and the device is online. Confirm
  or a tap marks an item not done, moving it to the end of the task list.
  Back returns to the task list without another sync.
- Lists are `UiListActivity` screens with row providers, as the project's
  UI rules require.

### Settings

The favourite is the first list starred in Vikunja (`is_favorite`), so it
can be set from any Vikunja client. Settings → Connectivity → **Favourite
list** offers "Starred in Vikunja" (the default) plus the projects in the
saved copy, to pick another on this device. A pick is stored by Vikunja
project id, not by its position in the list; one that no longer exists
falls back to the star. A list starred since the last sync is opened as
soon as the sync on entering Lists finds it.

### Sync

- **When**: one full sync per visit to Lists (on entry, or on opening the
  favourite), and again when the user picks the Sync row. A list's items
  are downloaded the first time it is opened in a visit; after that, and on
  Back, the saved copy shows at once with no network. Leaving Lists ends
  the visit. If no network is up, Lists joins Wi-Fi the way Sync library
  does, once; a failed join is not retried until the Sync row is picked or
  Lists is left.
- **Order**: reload projects, then send pending ticks, then the shown
  project's items.
- **Sending a tick**: fetch the task, change the top-level `done` value in
  the text as received (`setTopLevelDone`), and send it back. Vikunja's
  update replaces every field, so the whole task is sent; editing the text
  rather than re-serializing keeps number fields such as `position` exact.
  If the task is already in the wanted state, or has been deleted (404),
  drop the tick. If the update gets a 4xx other than 401 (a read-only
  share, say), drop it too: the same credentials just read the task, so it
  will never succeed, and it must not block the ticks behind it. On 401, a
  server error or no answer, keep it and stop sending for this sync. Sent
  ticks are written into the project's saved copy, so they stay ticked
  offline.
- **Pending ticks**: one file, `/.crosspoint/vikunja-pending.json`, holding
  task id, wanted done state and project id. It is written before the
  screen updates. Ticking the same item twice cancels the pending entry.
  If a sync fails partway, the list is redrawn from the saved copy, which
  already holds the ticks that were sent.
- **Saved copy**: under `/.crosspoint/vikunja/`: `projects.json` for the
  projects, and `p<id>.txt` per project for its tasks, one
  `id<TAB>0|1<TAB>title` line each (read without building a JSON tree).
  Pending ticks are applied on top when drawing, so the screen always shows
  the user's ticks.
- **Paging**: pages are read until one is empty or adds nothing new; a
  short page does not end the list, because a server may cap `per_page`
  below what was asked.
- **Errors** (shown on the status line, saved copy kept): no network,
  server not answering, token rejected (401/403), and redirected to a login
  page (Cloudflare Access refused the headers).

### Size

- No limit on projects or tasks. The saved copy is on the SD card, and
  responses are read a page at a time.
- Allocations under 4 KB come from the internal heap (about 97 KB free),
  not PSRAM. So a task list keeps all titles in one buffer plus one row
  array (`TaskList`), which are each one allocation and land in PSRAM once
  large, and API pages are parsed through an ArduinoJson filter that keeps
  only the fields used.
- Before each page, if the largest free PSRAM block is under 512 KB, stop,
  keep what was loaded and show "List incomplete".
- Titles wrap to two lines, then are cut off with an ellipsis (*my call*).

## API notes (checked against a Vikunja v0.24 server behind Cloudflare Access)

- Cloudflare Access lets requests through with the service token headers
  (`/api/v1/info` answers 200). Without them it answers 302 to
  `*.cloudflareaccess.com`.
- `GET /api/v1/projects?page=N&per_page=50`: an array of projects with
  `id`, `title`, `is_archived`, `parent_project_id` (0 for top level),
  `position` and `views` (`id`, `view_kind`, `filter`). Each project has
  `list`, `gantt`, `table` and `kanban` views.
- **Open items**: `GET /api/v1/projects/{id}/views/{listViewId}/tasks?page=N&per_page=50`.
  The list view's own filter is `done = false`, and a `filter` query
  parameter cannot show done items through it. Items come in the order
  the Vikunja list shows.
- **Done items**: the table view has no filter of its own:
  `GET /api/v1/projects/{id}/views/{tableViewId}/tasks?filter=done%20%3D%20true&sort_by=done_at&order_by=desc&page=N&per_page=50`.
- **Paging**: the `x-pagination-total-pages` header is sent, but
  `HttpDownloader` does not return headers. Instead, fetch pages until one
  is empty or adds nothing new.
- **Fields used**: tasks `id`, `title`, `done`; projects `id`, `title`,
  `parent_project_id`, `is_archived`, `is_favorite` and the view ids.
- **Update**: `POST /api/v1/tasks/{id}` with the full task JSON, as fetched
  from `GET /api/v1/tasks/{id}`, with `done` changed. Sending a task back
  unchanged returned 200 and left every field as it was. A body with only
  `done` was not tried, since it could wipe a real task.
- **Token permissions**: reading projects and tasks and updating tasks is
  enough; this feature never creates anything.
- **Sub-projects**: `parent_project_id` is set on them. The project list
  shows them under their parent, indented one step (*my call*).
- Synthetic fixtures with the same shape as these responses are in
  `test/vikunja/fixtures/`.

## Left to the builder

- The icon choice and layout metrics.
- Where the code lives, following the Home Assistant feature's split: a
  host-tested library (`lib/Vikunja/`: config parsing, JSON models, the
  pending-tick merge rules) and the device side in `src/`.

## Verification

- Host tests: config parsing and obfuscation rewrite; the task and project
  JSON models; pending-tick rules (tick, untick, double tick, already done
  on the server, deleted on the server).
- Simulator: the screens with a fake server, including every error line.
- Device (the user): their real instance, ticking items with Wi-Fi off,
  rebooting, then checking that the ticks reach Vikunja once online.
