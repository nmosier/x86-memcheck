#pragma once

class Terminator;

#include <memory>
#include <list>
#include <type_traits>
#include "inst.hh"
#include "block-pool.hh"
#include "ptr-pool.hh"
#include "rsb.hh"

class Terminator {
public:
  using InstVec = std::list<std::unique_ptr<Blob>>;
  using InstIt = InstVec::iterator;
  using LookupBlock = std::function<uint8_t *(uint8_t *)>;
  using ProbeBlock = std::function<uint8_t *(uint8_t *)>; // returns nullptr if block not present
  using BkptCallback = std::function<void(void)>;
  using RegisterBkpt = std::function<void(uint8_t *, const BkptCallback&)>;
  using UnregisterBkpt = std::function<void(uint8_t *)>;
  
  static Terminator *Create(BlockPool& block_pool, PointerPool& ptr_pool, const Instruction& branch,
			    Tracee& trace, const LookupBlock& lb, const ProbeBlock& pb,
			    const RegisterBkpt& rb, const ReturnStackBuffer& rsb);
  
  void handle_bkpt_singlestep(void); // handle breakpoint by single-stepping

protected:
  Terminator(BlockPool& block_pool, size_t size, const Instruction& branch,
	     Tracee& tracee, const LookupBlock& lb);

  uint8_t *write(uint8_t *addr, const uint8_t *data, size_t count);
  uint8_t *write(const Blob& blob) { return write(blob.pc(), blob.data(), blob.size()); }
  uint8_t *write(uint8_t *addr, uint8_t byte) { return write_i<uint8_t>(addr, byte); }
  uint8_t *write_bkpt(uint8_t *addr) { return write(addr, 0xcc); }
  
  void flush() const;

  template <typename... Args>
  uint8_t *lookup_block(Args&&... args) { return lb_(args...); }

  Tracee& tracee() const { return tracee_; }

  template <typename Derived>
  static BkptCallback make_callback(Derived *term,
				    void (Derived::*fn)(void)) {
    return [=] (void) {
      (term->*fn)();
    };
  }

  uint8_t *addr() const { return addr_; }

private:
  using Buf = std::vector<uint8_t>;
  uint8_t *addr_;
  size_t size_;
  Buf buf_;
  Tracee& tracee_;
  const LookupBlock lb_;
  uint8_t *orig_branch_addr;

  template <typename I>  
  uint8_t *write_i(uint8_t *addr, I i) {
    static_assert(std::is_integral<I>(), "require integral type");
    return write(addr, reinterpret_cast<const uint8_t *>(&i), sizeof(i));
  }
};

class DirJmpTerminator: public Terminator {
public:
  DirJmpTerminator(BlockPool& block_pool, const Instruction& jmp, Tracee& tracee,
		   const LookupBlock& lb);
private:
  static constexpr size_t DIR_JMP_SIZE = Instruction::jmp_relbrd_len;
};

class DirJccTerminator: public Terminator {
public:
  DirJccTerminator(BlockPool& block_pool, const Instruction& jcc, Tracee& tracee,
		   const LookupBlock& lb, const RegisterBkpt& rb);
private:
  static constexpr size_t DIR_JCC_SIZE =
    Instruction::jcc_relbrd_len + Instruction::jmp_relbrd_len + Instruction::int3_len;
  Instruction jcc_inst;
  uint8_t *orig_dst;
  uint8_t *orig_fallthru;
  uint8_t *jcc_bkpt_addr;
  uint8_t *fallthru_bkpt_addr;

  void handle_bkpt_fallthru(void);
  void handle_bkpt_jcc(void);
};

class IndTerminator: public Terminator {
public:
  IndTerminator(BlockPool& block_pool, const Instruction& branch, Tracee& tracee,
		const LookupBlock& lb, const RegisterBkpt& rb);
private:
  static constexpr size_t IND_SIZE = Instruction::int3_len;
};

class RetTerminator: public Terminator {
public:
  RetTerminator(BlockPool& block_pool, const Instruction& ret, Tracee& tracee,
		const LookupBlock& lb, const RegisterBkpt& rb, const ReturnStackBuffer& rsb);

private:
  static constexpr size_t RET_SIZE = 0x32; // from rsb-ret.asm.
};

class CallTerminator: public Terminator {
public:
  CallTerminator(BlockPool& block_pool, PointerPool& ptr_pool, size_t size, const Instruction& call,
		 Tracee& tracee, const LookupBlock& lb, const ProbeBlock& pb, const RegisterBkpt& rb,
		 const ReturnStackBuffer& rsb);

protected:
  uint8_t *subaddr() const { return Terminator::addr() + CALL_SIZE_PRE; }
  
private:
  static constexpr size_t CALL_SIZE_PRE = 0x2B; // from rsb-call.asm
  static constexpr size_t CALL_SIZE_POST = 1; // one breakpoint
  static constexpr size_t CALL_SIZE = CALL_SIZE_PRE + CALL_SIZE_POST;
  uint8_t *orig_ra_val;
  uint8_t **new_ra_ptr;

  void handle_bkpt_ret(void);  
};

class CallDirTerminator: public CallTerminator {
public:
  CallDirTerminator(BlockPool& block_pool, PointerPool& ptr_pool, const Instruction& call,
		    Tracee& tracee, const LookupBlock& lb, const ProbeBlock& pb,
		    const RegisterBkpt& rb, const ReturnStackBuffer& rsb);
  
private:
  static constexpr size_t CALL_DIR_SIZE = 11;
};

class CallIndTerminator: public CallTerminator {
public:
  CallIndTerminator(BlockPool& block_pool, PointerPool& ptr_pool, const Instruction& call,
		    Tracee& tracee, const LookupBlock& lb, const ProbeBlock& pb,
		    const RegisterBkpt& rb, const ReturnStackBuffer& rsb);
  
private:
  static constexpr size_t CALL_IND_SIZE = 1;
};

