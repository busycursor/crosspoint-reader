#pragma once

namespace vikunja {

// Outcome of the last sync, shown on the Lists status line.
enum class Status { Ok, Offline, ServerDown, TokenRejected, AccessRefused, Incomplete };

// httpStatus is 0 when no response arrived. A redirect to another origin
// is Cloudflare Access sending the request to its login page.
Status classifyResponse(int httpStatus, bool redirectedToOtherOrigin);

}  // namespace vikunja
