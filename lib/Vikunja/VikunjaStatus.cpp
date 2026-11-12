#include "VikunjaStatus.h"

namespace vikunja {

Status classifyResponse(const int httpStatus, const bool redirectedToOtherOrigin) {
  if (redirectedToOtherOrigin || (httpStatus >= 300 && httpStatus < 400)) return Status::AccessRefused;
  if (httpStatus == 0) return Status::Offline;
  if (httpStatus >= 200 && httpStatus < 300) return Status::Ok;
  if (httpStatus == 401 || httpStatus == 403) return Status::TokenRejected;
  return Status::ServerDown;
}

}  // namespace vikunja
