// Disposable Windows ARM64 CI smoke test for the Sunshine-pinned ViGEmClient.
// Attaches an X360 and a DS4 emulated device briefly, then removes them.
// Never intended to run on a production host without user consent.
#include <ViGEm/Client.h>
#include <cstdio>

static bool attach_and_remove(PVIGEM_CLIENT client, PVIGEM_TARGET target, const char *name) {
  if (!target) {
    std::fprintf(stderr, "Unable to allocate %s target\n", name);
    return false;
  }
  const auto status = vigem_target_add(client, target);
  if (!VIGEM_SUCCESS(status)) {
    std::fprintf(stderr, "ViGEm %s target attach error 0x%08X\n", name,
                 static_cast<unsigned>(status));
    vigem_target_free(target);
    return false;
  }
  std::printf("ViGEm %s virtual target attached\n", name);
  const auto removed = vigem_target_remove(client, target);
  vigem_target_free(target);
  if (!VIGEM_SUCCESS(removed)) {
    std::fprintf(stderr, "ViGEm %s target removal error 0x%08X\n", name,
                 static_cast<unsigned>(removed));
    return false;
  }
  std::printf("ViGEm %s virtual target removed\n", name);
  return true;
}

int main() {
  auto *client = vigem_alloc();
  if (!client) {
    std::fprintf(stderr, "vigem_alloc failed\n");
    return 1;
  }
  const auto status = vigem_connect(client);
  if (!VIGEM_SUCCESS(status)) {
    std::fprintf(stderr, "vigem_connect failed: 0x%08X\n",
                 static_cast<unsigned>(status));
    vigem_free(client);
    return 2;
  }
  const bool x360 = attach_and_remove(client, vigem_target_x360_alloc(), "X360");
  const bool ds4 = attach_and_remove(client, vigem_target_ds4_alloc(), "DS4");
  vigem_disconnect(client);
  vigem_free(client);
  if (!x360 || !ds4) {
    std::fprintf(stderr, "ViGEm native gamepad target smoke failed\n");
    return 3;
  }
  std::puts("X360 and DS4 native ARM64 ViGEmClient attach/remove passed");
  return 0;
}
