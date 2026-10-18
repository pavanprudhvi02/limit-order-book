// Emscripten entry point: exposes web::Session to JavaScript.
#include <emscripten/bind.h>

#include "session.h"

EMSCRIPTEN_BINDINGS(order_book) {
  emscripten::class_<lob::web::Session>("Session")
      .constructor<>()
      .function("submit", &lob::web::Session::Submit)
      .function("cancel", &lob::web::Session::Cancel)
      .function("modify", &lob::web::Session::Modify)
      .function("simulate", &lob::web::Session::Simulate)
      .function("snapshot", &lob::web::Session::Snapshot)
      .function("reset", &lob::web::Session::Reset)
      .function("invariants", &lob::web::Session::Invariants);
}
