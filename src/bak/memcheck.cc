#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sstream>
#include "memcheck.hh"
#include "syscall-check.hh"

bool Memcheck::open(const char *file, char * const argv[]) {
  const pid_t child = fork();
  if (child == 0) {
    ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
    execvp(file, argv);
    return false;
  }
  
  Decoder::Init();
  
  int status;
  wait(&status);
  assert(stopped_trace(status));

  tracee.open(child, file);
  // TODO: open patcher
  patcher = Patcher(tracee, [this] (auto&&... args) { return this->transformer(args...); });
  patcher->start();
  maps_gen.open(child);
  tracked_pages.add_state(taint_state);
  tracked_pages.add_maps(maps_gen);

  memory = UserMemory(tracee, PAGESIZE, PROT_READ | PROT_WRITE);
  
  syscall_tracker = SyscallTracker(tracee, tracked_pages, taint_state, stack_begin(), *this);
    
  // patcher->signal(SIGSEGV, [this] (int signum) { segfault_handler(signum); });
  save_state(pre_state);
  init_taint(taint_state);

  return true;
}


bool Memcheck::stopped_trace(int status) {
  return WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP;  
}


bool Memcheck::is_sp_dec(const Instruction& inst) {
  if (inst.xed_reg0() != XED_REG_RSP) {
    return false;
  }

  switch (inst.xed_iclass()) {
  case XED_ICLASS_PUSH: return false;
  default:
    break;
  }

  return true; // TODO: Should be more conservative about this...
}

bool Memcheck::is_jcc(const Instruction& inst) {
  switch (inst.xed_iclass()) {
  case XED_ICLASS_JB:
  case XED_ICLASS_JBE:
  case XED_ICLASS_JCXZ:
  case XED_ICLASS_JECXZ:
  case XED_ICLASS_JL:
  case XED_ICLASS_JLE:
  case XED_ICLASS_JNB:
  case XED_ICLASS_JNBE:
  case XED_ICLASS_JNL:
  case XED_ICLASS_JNLE:
  case XED_ICLASS_JNO:
  case XED_ICLASS_JNP:
  case XED_ICLASS_JNS:
  case XED_ICLASS_JNZ:
  case XED_ICLASS_JO:
  case XED_ICLASS_JP:
  case XED_ICLASS_JRCXZ:
  case XED_ICLASS_JS:
  case XED_ICLASS_JZ:
    return true;
  default:
    return false;
  }
}

void Memcheck::transformer(uint8_t *addr, Instruction& inst, const Patcher::TransformerInfo& info) {
  (void) addr;

#if 1
  if (is_sp_dec(inst)) {
    addr = stack_tracker.add(addr, inst, info);
    return;
  }
#endif

#if 1
  if (inst.xed_iclass() == XED_ICLASS_SYSCALL) {
    addr = syscall_tracker->add(addr, inst, info, 
			       util::method_callback(this, &Memcheck::syscall_handler_pre));
    return;
  }
#endif

#if 1
  if (inst.xed_iclass() == XED_ICLASS_CALL_NEAR || inst.xed_iclass() == XED_ICLASS_RET_NEAR) {
    addr = call_tracker.add(addr, inst, info);
    return;
  }
#endif

#if 1
  if (is_jcc(inst)) {
    addr = jcc_tracker.add(addr, inst, info);
    return;
  }
#endif
  
  addr = info.writer(inst);
}

void Memcheck::run() {
  patcher->run();
}

void Memcheck::segfault_handler(int signum) {
  const siginfo_t siginfo = tracee.get_siginfo();

  void *fault_addr = siginfo.si_addr;
  std::cerr << "segfault @ " << fault_addr << std::endl;

  tracee.syscall(Syscall::MPROTECT, (uintptr_t) mprotect_ptr(fault_addr), mprotect_size, PROT_READ | PROT_WRITE);
}

void Memcheck::save_state(State& state) {
  state.save(tracee, tracked_pages.begin(), tracked_pages.end());
}

State Memcheck::save_state() {
  State state;
  save_state(state);
  return state;
}

void *Memcheck::stack_begin() {
  const auto stack_end = pagealign_up(tracee.get_sp());
  auto stack_begin = stack_end;
  do {
    stack_begin = pageidx(stack_begin, -1);
  } while (tracked_pages.find(stack_begin) != tracked_pages.end());
  stack_begin = pageidx(stack_begin, 1);
  return stack_begin;
}

/* Rewind to pre_state, flipping bits in taint_state */
void Memcheck::set_state_with_taint(State& state, const State& taint) {
  state ^= taint;
  state.restore(tracee);
}

template <typename InputIt>
void Memcheck::update_taint_state(InputIt begin, InputIt end, State& taint_state) {
  assert(std::distance(begin, end) >= 2);

  auto first = begin++;

  assert(std::all_of(begin, end, std::bind(&State::similar, first, std::placeholders::_1)));
  
  /* taint stack */
  init_taint(taint_state); // TODO: Could be optimized.

  for (auto it = begin; it != end; ++it) {
    taint_state |= *first ^ *it; // TODO: could be optimized.
  }
}

#define ABORT_ON_TAINT 1

void Memcheck::check_round() {
  // TODO: should return bool.
  
  /* get taint mask */
  update_taint_state(post_states.begin(), post_states.end(), taint_state);

  // TODO: DEBUG:
  {
    const auto begin1 = jcc_lists[0].begin(), end1 = jcc_lists[0].end();
    const auto begin2 = jcc_lists[1].begin(), end2 = jcc_lists[1].end();
    auto it1 = begin1, it2 = begin2;
    for (; it1 != end1 && it2 != end2; ++it1, ++it2) {
      if (*it1 != *it2) {
	assert(it1->first == it2->first);
	std::clog << "JCC MISMATCH @ " << (void *) it1->first << ", flags " << std::hex
		  << it1->second << " vs " << std::hex << it2->second << "\n";
	abort();
      }
    }
    if (it1 != end1) {
      std::clog << "JCC OVERHANG @ " << (void *) it1->first << ", flags " << std::hex << it1->second
		<< "\n";
      abort();
    }
    if (it2 != end2) {
      std::clog << "JCC OVERHANG @ " << (void *) it2->first << ", flags " << std::hex << it2->second
		<< "\n";
      abort();
    }
    if (!util::all_equal(jcc_lists.begin(), jcc_lists.end())) {
      std::clog << "memcheck: condition jump maps differ\n";
      abort();
    }
  }

  /* ensure eflags cksum same */
  if (!util::all_equal(jcc_cksums.begin(), jcc_cksums.end())) {
    std::clog << "memcheck: conditional jump checksums differ\n";
    abort();
  }

  /* make sure args to syscall aren't tainted */
  SyscallChecker syscall_checker
    (tracee, taint_state, AddrRange(stack_begin(), static_cast<char *>(tracee.get_sp()) -
				    SHADOW_STACK_SIZE), syscall_args, *this);
  if (!syscall_checker.pre()) {
    /* DEBUG: Translate */
    const auto orig_addr = patcher->orig_block_addr(tracee.get_pc());
    std::clog << "orig addr: " << (void *) orig_addr << "\n";
    if (g_conf.gdb) {
      tracee.set_pc(tracee.get_pc() + 10);
      tracee.gdb();
    } else if (ABORT_ON_TAINT) {
      abort();
    }
  }
}



void Memcheck::init_taint(State& taint_state) {
  /* taint memory below stack */
  save_state(taint_state); // TODO: optimize
  taint_state.zero();

  taint_state.fill(stack_begin(), static_cast<char *>(tracee.get_sp()) - SHADOW_STACK_SIZE, -1);
}

void Memcheck::start_subround() {
  save_state(pre_state);
  if (CHANGE_PRE_STATE) {
    set_state_with_taint(pre_state, taint_state);
  }
}

void Memcheck::stop_subround() {
  
}
