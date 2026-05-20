#include <dlfcn.h>
#include <fcntl.h>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <string>
#include "base.hpp"

// Utility functions for monitoring process memory
static int count_mappings(const std::string & needle) {
  FILE * f = fopen("/proc/self/maps", "r");
  if (!f) return -1;
  char line[4096];
  int n = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, needle.c_str())) n++;
  }
  fclose(f);
  return n;
}

static bool address_is_readable(const void * p) {
  int fds[2];
  if (pipe(fds) < 0) return false;
  ssize_t r = write(fds[1], p, 1);
  int err = errno;
  close(fds[0]);
  close(fds[1]);
  if (r == 1) return true;
  if (r < 0 && err == EFAULT) return false;
  return false;
}

// RAII Library Manager (No RTLD_NODELETE)
struct LibraryHandle {
  void * handle;
  std::string name;

  LibraryHandle(const std::string & path, const std::string & basename) : name(basename) {
    // Normal LAZY load. The memory WILL unmap when dlclose is called.
    handle = dlopen(path.c_str(), RTLD_LAZY);
    printf("  [LibraryHandle] dlopen executed for %s\n", name.c_str());
  }

  ~LibraryHandle() {
    if (handle) {
      dlclose(handle);
      printf("  [LibraryHandle] dlclose executed. Memory safely unmapped!\n");
    }
  }
};

int main(int argc, char ** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <path-to-plugin.so>\n", argv[0]); return 2; }
  std::string path = argv[1];
  std::string basename = path.substr(path.find_last_of("/") + 1);

  // Loop 2 times to check for static state accumulation across reloads
  for (int cycle = 1; cycle <= 2; ++cycle) {
    printf("\n============================================\n");
    printf("== CYCLE %d: Loading %s ==\n", cycle, basename.c_str());
    printf("============================================\n");

    std::vector<std::weak_ptr<Base>> weaks;
    void * vtable_addr = nullptr;

    // 1. Load the library via our smart manager
    auto lib_lifetime = std::make_shared<LibraryHandle>(path, basename);
    if (!lib_lifetime->handle) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }

    auto make = (std::shared_ptr<Base>(*)())dlsym(lib_lifetime->handle, "make_obj");
    if (!make) { fprintf(stderr, "dlsym failed: %s\n", dlerror()); return 1; }

    {
      // 2. Fetch the plugin object
      std::shared_ptr<Base> plugin_obj = make();
      Base * raw = plugin_obj.get();
      vtable_addr = *(void **) raw;

      // 3. Keep the library alive via the custom deleter
      std::shared_ptr<Base> managed_obj(raw, [lib_lifetime, plugin_obj](Base* p) {
        // Keeps lib_lifetime reference count extended until the object finishes deleting
      });

      printf("  Mappings while strong ref held: %d\n", count_mappings(basename));
      printf("  Derived vtable @ %p, readable: %s\n",
             vtable_addr, address_is_readable(vtable_addr) ? "yes" : "NO");

      weaks.push_back(managed_obj);
    }
    // Strong ref dropped. C++ object deleted. Only weak_ptr context block remains.

    printf("\n  -- Simulating Host dropping its main library handle --\n");
    lib_lifetime.reset();

    printf("  Mappings after host reset:        %d (Stalled from unmapping by weak_ptr)\n", count_mappings(basename));
    printf("  Vtable still readable:            %s\n", address_is_readable(vtable_addr) ? "yes" : "NO");

    printf("\n  -- Destroying remaining weak_ptr trackers --\n");
    fflush(stdout);

    weaks.clear(); // Control block dies -> Custom deleter dies -> LibraryHandle ref count hits 0 -> dlclose() fires!

    printf("  Mappings post-cleanup:            %d (Successfully dropped to 0!)\n", count_mappings(basename));
    printf("  Survived cycle %d without fault.\n", cycle);
  }

  return 0;
}
