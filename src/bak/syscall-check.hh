#pragma once

class SyscallChecker;

#include "syscall.hh"
#include "addr-range.hh"
#include "state.hh"
#include "syscall-args.hh"
class Memcheck;

// checks and propogates taint
class SyscallChecker {
public:
  SyscallChecker(Tracee& tracee, State& taint_state, void *stack_begin, const SyscallArgs& args,
		 const Memcheck& memcheck):
    tracee(tracee), taint_state(taint_state), stack_begin(stack_begin), args(args),
    taint_args(taint_state.regs()), memcheck(memcheck) {}

  bool pre();
  void post();
  
  template <typename... Args>
  bool operator()(Args&&... args) { return run(args...); }
  
private:
  Tracee& tracee;
  State& taint_state;
  void *stack_begin;
  const SyscallArgs& args;
  SyscallArgs taint_args;
  const Memcheck& memcheck;

  AddrRange stack_range();

bool check_read(const void *begin, const void *end) const;
  bool check_read(const void *begin, size_t size) const {
    return check_read(begin, static_cast<const char *>(begin) + size);
  }
  bool check_read(const char *s);

  bool check_write(void *begin, void *end) const;
  bool check_write(void *begin, size_t size) const;

  void do_write(void *begin, void *end);
  void do_write(void *addr, size_t size);
  void do_write(char *str);

  template <class S>
  void read_struct(const void *addr, S& s) {
    tracee.read(&s, sizeof(S), addr);
  }

  template <typename T>
  void print_values(const T *addr) const;

  template <size_t N, typename T>
  void print_regs() const;
  
  enum class Access {NONE, READ, WRITE};
  static constexpr Access access(const char *typestr);

#define SYSCALLS_CHECK_PRE_DECL(name, ...) bool pre_##name();
  SYSCALLS(SYSCALLS_CHECK_PRE_DECL)
#undef SYSCALLS_CHECK_PRE_DECL
  
#define SYSCALLS_CHECK_POST_DECL(name, ...) void post_##name();
  SYSCALLS(SYSCALLS_CHECK_POST_DECL)
#undef SYSCALLS_CHECK_POST_DECL
  
};
