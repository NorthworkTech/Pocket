/** Reading library: unique files per cloud id, real titles across reloads, edits re-download,
 * removed books leave, CRLF/BOM cleaned, hard wraps unwrapped, odd titles parse. */
#include "pocket/app.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace pocket;

static int failures = 0;
#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures; \
    } \
  } while (0)

struct TClock : PlatformClock {
  uint32_t now_ms() override { return 1000; }
  void local_hm(int& h, int& m, int& wd, int& mo, int& d) override { h = 10; m = 0; wd = 1; mo = 3; d = 24; }
};
struct TWifi : PlatformWifi {
  std::vector<std::string> scan() override { return {}; }
  bool connect(const std::string&, const std::string&) override { return true; }
  bool connected() const override { return true; }
  bool start_provision(const std::string&, std::string*, std::string*) override { return false; }
  void stop_provision() override {}
  bool take_provision_credentials(std::string*, std::string*) override { return false; }
  std::string provision_ap_ssid() const override { return {}; }
  std::string provision_ap_password() const override { return {}; }
};
struct TCloud : PlatformCloud {
  std::string list;
  std::map<std::string, std::string> bodies;
  int downloads = 0;
  std::string create_pair_session(const std::string&) override { return {}; }
  std::string pair_status(const std::string&) override { return "pending"; }
  void refresh_entitlement(DeviceConfig&) override {}
  std::string stt_transcribe(const std::vector<uint8_t>&) override { return {}; }
  std::string books_list_json(const std::string&) override { return list; }
  std::vector<uint8_t> book_download(const std::string&, const std::string& id) override {
    ++downloads;
    const std::string& b = bodies[id];
    return std::vector<uint8_t>(b.begin(), b.end());
  }
};
struct TDisp : PlatformDisplay {
  void present(const Canvas&, RefreshMode) override {}
};
struct TStorage : PlatformStorage {
  std::string root = "/tmp/pocket-books-library-test";
  std::string book_root() override { return root; }
  bool book_ensure_root() override { return std::system(("mkdir -p " + root).c_str()) == 0; }
  uint64_t free_bytes(const std::string&) override { return 64ull * 1024 * 1024; }
};

static int find(App& app, const std::string& title) {
  auto& b = app.host_books();
  for (size_t i = 0; i < b.size(); ++i)
    if (b[i].title == title) return static_cast<int>(i);
  return -1;
}

int main() {
  MemoryConfigStore store;
  store.mut().onboarding_complete = true;
  store.mut().device_id = "dev";
  store.mut().pin_length = 4;
  set_pin(store.mut(), "0000");
  TClock clock; TWifi wifi; TCloud cloud; TDisp disp; TStorage storage;
  std::system(("rm -rf " + storage.root + " && mkdir -p " + storage.root).c_str());
  // A ≤2.0 download under the old server-filename scheme — must not be listed twice.
  std::system(("printf 'old' > " + storage.root + "/book.txt").c_str());

  std::string gutenberg = "\xEF\xBB\xBF";
  for (int p = 0; p < 6; ++p) {
    for (int l = 0; l < 6; ++l) gutenberg += "It was a bright cold day in April, and the clocks were striking.\r\n";
    gutenberg += "\r\n";
  }
  cloud.bodies["id-emma"] = gutenberg;
  cloud.bodies["id-dune"] = "Dune text.\nA spice.";
  cloud.list =
      R"({"books":[{"id":"id-emma","title":"Emma \"Annotated\" {1815}","author":"Jane Austen","format":"txt","filename":"book.txt","size_bytes":100},)"
      R"({"id":"id-dune","title":"Dune","author":"Frank Herbert","format":"epub","filename":"book.txt","size_bytes":200}]})";

  App app(store, clock, wifi, cloud, disp, &storage, nullptr);
  app.boot();
  app.host_reading_sync();
  CHECK(cloud.downloads == 2);
  CHECK(app.host_books().size() == 2);
  const int emma = find(app, "Emma \"Annotated\" {1815}");
  const int dune = find(app, "Dune");
  CHECK(emma >= 0 && dune >= 0);
  if (emma >= 0) CHECK(app.host_books()[static_cast<size_t>(emma)].author == "Jane Austen");

  // Same server filename, different books: each opens its own text.
  if (dune >= 0) {
    app.host_reading_open(dune);
    CHECK(app.host_reading_text().find("Dune text.") == 0);
    app.handle(InputEvent::Back);
  }
  if (emma >= 0) {
    app.host_reading_open(emma);
    const std::string& t = app.host_reading_text();
    CHECK(t.find('\r') == std::string::npos);            // CRLF cleaned
    CHECK(t.rfind("It was", 0) == 0);                    // BOM gone
    CHECK(t.find("striking. It was") != std::string::npos);  // hard wrap unwrapped
    CHECK(t.find("\n\n") != std::string::npos);          // paragraphs kept
    app.handle(InputEvent::Back);
  }

  // Reload from disk (e.g. next boot): titles/authors survive.
  app.host_reading_load();
  CHECK(app.host_books().size() == 2);
  CHECK(find(app, "Dune") >= 0);

  // Unchanged → no re-download; edited (size changed) → re-download; removed → deleted.
  cloud.downloads = 0;
  app.host_reading_sync();
  CHECK(cloud.downloads == 0);
  cloud.bodies["id-dune"] = "Dune, revised.";
  cloud.list = R"({"books":[{"id":"id-dune","title":"Dune","author":"Frank Herbert","format":"epub","size_bytes":201}]})";
  app.host_reading_sync();
  CHECK(cloud.downloads == 1);
  CHECK(app.host_books().size() == 1);
  if (!app.host_books().empty()) {
    app.host_reading_open(0);
    CHECK(app.host_reading_text() == "Dune, revised.");
    app.handle(InputEvent::Back);
  }

  // A garbled reply never wipes the library.
  cloud.list = "<html>502</html>";
  app.host_reading_sync();
  CHECK(app.host_books().size() == 1);

  // Wrapped text: a single newline is a line break, not a blank line.
  {
    Canvas c;
    const int y1 = c.draw_text_wrapped(0, 0, 400, 0, "A\nB", Canvas::TextRole::Body, Gray::G0);
    const int y2 = c.draw_text_wrapped(0, 0, 400, 0, "A\n\nB", Canvas::TextRole::Body, Gray::G0);
    const int lh = c.text_height(Canvas::TextRole::Body);
    CHECK(y1 == 2 * lh);
    CHECK(y2 == 3 * lh);
  }

  if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
  std::puts("test_books_library OK");
  return 0;
}
