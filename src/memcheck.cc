#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sstream>
#include "memcheck.hh"
#include "syscall-check.hh"
#include "flags.hh"

const Memcheck::RoundArray<uint8_t> Memcheck::fills = {{0x00, 0xff}};

Memcheck::Memcheck():
  tracee(),
  patcher(),
  vars(),
  stack_tracker(tracee, 0),
  syscall_tracker(tracee,
		  SequencePoint(taint_state,
				[this] (auto addr) {
				  this->sequence_point_handler_pre(syscall_tracker);
				},
				[this] (auto addr) { this->start_round(); }
				),
		  tracked_pages,
		  syscall_args,
		  *this
		  ),
  call_tracker(tracee, 0),
  jcc_tracker(tracee, cksum, vars),
  lock_tracker(tracee,
	       SequencePoint(taint_state,
			     [this] (auto addr) {
			       this->sequence_point_handler_pre(lock_tracker);
			     },
			     [this] (auto addr) { this->start_round(); }
			     )
	       ),
  rtm_tracker(tracee,
	      SequencePoint(taint_state,
			    [this] (auto addr) {
			      this->sequence_point_handler_pre(rtm_tracker);
			    },
			    [this] (auto addr) { this->start_round(); }
			    )
	      ),
  rdtsc_tracker(taint_state,
		[this] (auto addr) {
		  this->sequence_point_handler_pre(rdtsc_tracker);
		},
		[this] (auto addr) { this->start_round(); },
		tracee
		)
    
{}
  

void Memcheck::write_maps() const {
  if (g_conf.map_file) {
    std::stringstream ss;
    ss << "/proc/" << tracee.pid() << "/maps";
    std::ifstream maps_in(ss.str());
    std::string line;
    while (std::getline(maps_in, line)) {
      g_conf.map_file << line << "\n";
    }
    g_conf.map_file.close();
  }
}

Memcheck *Memcheck::cur_memcheck = nullptr;

bool Memcheck::open(const char *file, char * const argv[]) {
  cur_memcheck = this;
  
  const pid_t child = fork();
  if (child == 0) {
    if (g_conf.preload) {
      char *dirname = get_current_dir_name();
      if (dirname == nullptr) {
	std::perror("get_current_dir_name");
	std::abort();
      }
      std::stringstream ss;
      ss << dirname << "/" << "libmemcheck-libc.so";
      setenv("LD_PRELOAD", ss.str().c_str(), true);
      assert(getenv("LD_PRELOAD") != nullptr);
    }
    
    ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
    execvp(file, argv);
    return false;
  }

  signal(SIGINT, sigint_handler);

  Decoder::Init();

  int status;
  wait(&status);
  assert(stopped_trace(status));

  tracee.open(child, file);
  // TODO: open patcher
  patcher = Patcher(tracee, [this] (auto&&... args) { return this->transformer(args...); });

  vars.open(tracee, *patcher);

  patcher->start();

  maps_gen.open(child);
  tracked_pages.add_state(taint_state);
  tracked_pages.add_maps(maps_gen);
   

  // patcher->signal(SIGSEGV, [this] (int signum) { segfault_handler(signum); });
  patcher->signal(SIGSTOP, sigignore);
  patcher->signal(SIGCONT, sigignore);
  patcher->signal(SIGINT,  sigignore);
  patcher->signal(SIGTSTP, sigignore);
  patcher->sigaction(SIGSEGV, [this] (auto&&... args) { this->segfault_handler(args...); });
  
  save_state(pre_state);
  init_taint(taint_state, true); // also taint shadow stack

  // TEMP
  protect_map("[vdso]", PROT_READ);
  protect_map("[vvar]", PROT_NONE);

  vars.init_for_subround(cur_fill());

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
  
  if (inst.xed_iclass() == XED_ICLASS_SYSCALL) {
    addr = syscall_tracker.add(addr, inst, info);
    return;
  }
  
  if (CALL_TRACKER) {
    if (inst.xed_iclass() == XED_ICLASS_CALL_NEAR || inst.xed_iclass() == XED_ICLASS_RET_NEAR) {
      addr = call_tracker.add(addr, inst, info);
      return;
    }
  }

  if (JCC_TRACKER) {
    if (is_jcc(inst)) {
      addr = jcc_tracker.add(addr, inst, info);
      return;
    }
  }

  if (LOCK_TRACKER) {
    bool is_lock;
    addr = lock_tracker.add(addr, inst, info, is_lock);
    if (is_lock) { return; }
  }

  if (RDTSC_TRACKER) {
    bool is_rdtsc = false;
    addr = rdtsc_tracker.add(addr, inst, info, is_rdtsc);
    if (is_rdtsc) { return; }
  }

  // DEBUG
  if (inst.xed_iclass() == XED_ICLASS_RDTSC) {
    auto bkpt = Instruction::int3(addr);
    addr = info.writer(bkpt);
    info.rb(bkpt.pc(), [] (const auto addr) {
      *g_conf.log << "RDTSC @ " << (void *) addr << "\n";
    });
    addr = info.writer(inst);
    return;
  }
  
  addr = info.writer(inst);
}

void Memcheck::run() {
  start_round();
  patcher->run();
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
  // g_conf.assert_(taint_state.is_zero(), tracee);
  
  assert(std::distance(begin, end) >= 2);
  
  auto first = begin++;

  assert(std::all_of(begin, end, std::bind(&State::similar, first, std::placeholders::_1)));
  
  /* taint stack */
  init_taint(taint_state, false); // TODO: Could be optimized.

  for (auto it = begin; it != end; ++it) {
    taint_state |= *first ^ *it; // TODO: could be optimized.
  }

  //  g_conf.assert_(taint_state.is_zero(), tracee);  
}

template <class SequencePoint>
void Memcheck::check_round(SequencePoint& seq_pt) {
  // TODO: should return bool.
  
  /* get taint mask */
  update_taint_state(post_states.begin(), post_states.end(), taint_state);

  // TODO: DEBUG:
  if (JCC_TRACKER_BKPT) {
    util::for_each_pair(bkpt_cksums.begin(), bkpt_cksums.end(),
			[this] (const auto& l, const auto& r) {
      l.diff(r, [this] (const auto addr,
			const auto& flags1, const auto& flags2,
			const auto& data1, const auto& data2) {
	*g_conf.log << "JCC MISMATCH @ " << (void *) addr << ", flags ";
	for (auto flag : FlagSet(flags1)) {
	  *g_conf.log << flag << " ";
	}
	*g_conf.log << "vs ";
	for (auto flag : FlagSet(flags2)) {
	  *g_conf.log << flag << " ";
	}
	*g_conf.log << "\n";
	const auto loc = this->orig_loc(addr);
	*g_conf.log << loc.first << " " << loc.second << "\n";
	*g_conf.log << "orig: " << Instruction((uint8_t *) loc.first, tracee) << "\n";
	*g_conf.log << "new:  " << Instruction((uint8_t *) addr + 1, tracee) << "\n";
	*g_conf.log << "data: " << data1 << " vs " << data2 << "\n";
	if (ABORT_ON_TAINT) {
	  g_conf.abort(tracee);
	}
      });
    });

    check_checksums(bkpt_cksums, "BKPT");
    
  }

  if (JCC_TRACKER_INCORE) {
    check_checksums(incore_cksums, "INCORE");
  }

  if (JCC_TRACKER_INCORE && JCC_TRACKER_BKPT) {
    /* ensure cksums agree */
    if (!std::equal(bkpt_cksums.begin(), bkpt_cksums.end(), incore_cksums.begin(),
		    incore_cksums.end(),
		    [] (const auto& bkpt_cksum, auto incore_cksum) {
		      return bkpt_cksum.cksum() == incore_cksum;
		    }))
      {
	*g_conf.log << "JCC incore and bkpt checksums disagree\n";
	g_conf.abort(tracee);
      }
  }

  seq_pt.check();
}

template <typename InputIt>
void Memcheck::check_checksums(InputIt begin, InputIt end, const char *desc) {
  if (!util::all_equal(begin, end)) {
    *g_conf.log << "memcheck: conditional jump checksums differ";
    if (desc) {
      *g_conf.log << " (" << desc << ")";
    }
    *g_conf.log << "\n";
    if (ABORT_ON_TAINT) {
      g_conf.abort(tracee);
    }
  }
}

Memcheck::Loc Memcheck::orig_loc(uint8_t *addr) {
  const auto orig_addr = patcher->orig_block_addr(addr);
  std::vector<Map> maps;
  maps_gen.get_maps(std::back_inserter(maps));
  for (const auto& map : maps) {
    if (map.has_addr(orig_addr)) {
      return {orig_addr, map.desc};
    }
  }
  abort();
}

void Memcheck::start_subround() {
  if (!CHANGE_PRE_STATE) {
    pre_state.restore(tracee);
  } else {
    set_state_with_taint(pre_state, taint_state);
  }
  
  stack_tracker.fill(cur_fill());
  call_tracker.fill(cur_fill());
  cksum.clear(); // TODO: rename cksum -> cksum
  vars.init_for_subround(cur_fill());
}

void Memcheck::stop_subround() {
  save_state(cur_post_state());

  if (JCC_TRACKER_BKPT) {
    cur_bkpt_cksum() = cksum;
  }
  if (JCC_TRACKER_INCORE) {
    cur_incore_cksum() = vars.jcc_cksum_val();
  }
}

// returns whether next subround exists
bool Memcheck::next_subround() {
  stop_subround();
  ++subround_counter;
  const bool success = subround_counter < SUBROUNDS;
  if (success) {
    start_subround();
  }
  return success;
}

void Memcheck::start_round() {  
  subround_counter = 0;
  save_state(pre_state);
  if (CHANGE_PRE_STATE) {
    set_state_with_taint(pre_state, taint_state);
  }
  start_subround();
  
  assert_taint_zero();
  assert(vars.jcc_cksum_val() == 0U);
}

void Memcheck::stop_round() {
  // TODO
}

template <typename SequencePoint>
void Memcheck::sequence_point_handler_pre(SequencePoint& seq_pt) {
  const bool has_next_subround = next_subround();
  if (!has_next_subround) {
    stop_round();
    check_round(seq_pt);
  }
}

void Memcheck::sequence_point_handler_post() {
  start_round();
}


void Memcheck::init_taint(State& taint_state, bool taint_shadow_stack) {
  /* taint memory below stack */
  save_state(taint_state); // TODO: optimize
  taint_state.zero();
  if (TAINT_STACK) {
    const auto padding = taint_shadow_stack ? 0 : SHADOW_STACK_SIZE;
    taint_state.fill(stack_begin(), static_cast<char *>(tracee.get_sp()) - padding, -1);
  }
}

void Memcheck::sigint_handler(int signum) {
  cur_memcheck->write_maps();
  std::exit(0);
}

void Memcheck::protect_map(const std::string& name, int prot) {
  std::vector<Map> maps;
  maps_gen.get_maps(std::back_inserter(maps));
  for (const auto& map : maps) {
    if (map.desc == name) {
      tracee.syscall(Syscall::MPROTECT, (uintptr_t) map.begin, (uintptr_t) map.size(), prot);
      return;
    }
  }
  abort();
}

void Memcheck::segfault_handler(int signal, const siginfo_t& siginfo) {
  // DEBUG
#if 0
  g_conf.singlestep = true;
  g_conf.execution_trace = true;
#endif
  
  const auto loc = orig_loc(tracee.get_pc());
  *g_conf.log << loc.first << " " << loc.second << "\n";
  *g_conf.log << "orig inst: " << Instruction((uint8_t *) loc.first, tracee) << "\n";
  *g_conf.log << "pool inst: " << Instruction(tracee.get_pc(), tracee) << "\n";
  *g_conf.log << "fault addr: " << siginfo.si_addr << "\n";

  // EXPERIMENTAL
  // will need to determine which permissions to use
  SharedMemSeqPt seq_pt(tracee, *this, taint_state);
  sequence_point_handler_pre(seq_pt);
}
