#include <dlfcn.h>
#include <fcntl.h>
#include <memory>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <string>
#include "base.hpp"

// Read /proc/self/maps and return count of lines mentioning needle.
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

// Reliable readability check via pipe syscall
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

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <path-to-plugin.so> [--nodelete]\n", argv[0]);
    return 2;
  }
  std::string path = argv[1];
  std::string basename = path.substr(path.find_last_of("/") + 1);

  // Determine flag configuration based on command-line arguments
  int flags = RTLD_LAZY;
  if (argc >= 3 && std::string(argv[2]) == "--nodelete") {
    flags |= RTLD_NODELETE;
    printf("== Loading %s WITH RTLD_NODELETE (Workaround Mode) ==\n", basename.c_str());
  } else {
    printf("== Loading %s WITHOUT RTLD_NODELETE (Bug Exposure Mode) ==\n", basename.c_str());
  }

  void * h = dlopen(path.c_str(), flags);
  if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

  auto make = (std::shared_ptr<Base>(*)())dlsym(h, "make_obj");
  if (!make) { fprintf(stderr, "dlsym: %s\n", dlerror()); return 1; }

  std::vector<std::weak_ptr<Base>> weaks;
  void * vtable_addr = nullptr;
  {
    std::shared_ptr<Base> p = make();
    Base * raw = p.get();
    vtable_addr = *(void **) raw;     // vtable pointer at offset 0 of the object
    printf("  mappings while strong ref held: %d\n", count_mappings(basename));
    printf("  Derived vtable @ %p, readable: %s\n",
           vtable_addr, address_is_readable(vtable_addr) ? "yes" : "NO");
    weaks.push_back(p);
  }
  // Strong ref dropped. Object destroyed. weak_count = 1.

  printf("\n== Strong ref dropped; weak_ptr alive. Calling dlclose() ==\n");
  int rc = dlclose(h);
  printf("  dlclose returned: %d (refcount drop succeeded)\n", rc);

  int after = count_mappings(basename);
  bool readable = address_is_readable(vtable_addr);
  printf("  mappings after dlclose:           %d\n", after);
  printf("  vtable still readable:            %s\n", readable ? "yes" : "NO");

  printf("\n== Destroying weak_ptr (calls _M_destroy via vtable lookup) ==\n");
  fflush(stdout);

  // Clang will crash here without --nodelete (provided unique symbols are stripped/absent)
  weaks.clear();

  printf("  Survived. No fault.\n");
  return 0;
}
