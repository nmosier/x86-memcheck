#include <cassert>
#include <algorithm>
#include <cstring>
#include "block.hh"
#include "block-pool.hh"

size_t Block::size(const InstVec& insts) {
  return std::accumulate(insts.begin(), insts.end(), 0,
			 [] (size_t acc, const auto& inst) {
			   return acc + inst->size();
			 });
}

Block *Block::Create(uint8_t *orig_addr, const Tracee& tracee, BlockPool& block_pool) {
  Block *block = new Block(tracee, orig_addr, block_pool);

  uint8_t *it = orig_addr;
  uint8_t *newit = block->peek();
  std::unique_ptr<Instruction> inst;

  /* get non-branch instructions */
  while (true) {
    inst = std::make_unique<Instruction>(it, tracee);
    assert(*inst);

    it += inst->size(); // update original PC
    
    block->kind_ = classify_inst(*inst);
    
    if (block->kind_ != Kind::OTH) {
      break;
    }

    /* relocate instruction and add to list */
    inst->relocate(newit);
    newit += newinst->size();
    block->insts_.push_back(std::move(inst));
  }

  block->orig_branch_ = *inst;

  /* create terminator instructions */
  
  
  // TODO
  abort();
  
  block->branch_insts_.push_back
    (std::make_unique<Instruction>(nullptr, Instruction::Data {0xcc}));
  block->branch_insts_.push_back
    (std::make_unique<Data>(Data::Content {static_cast<uint8_t>(BkptKind::BRANCH)}));
  
  block->fallthrough_insts_.push_back
    (std::make_unique<Instruction>(nullptr, Instruction::Data {0xcc}));
  block->fallthrough_insts_.push_back
    (std::make_unique<Data>(Data::Content {static_cast<uint8_t>(BkptKind::FALLTHROUGH)}));
  
  /* allocate space */
  constexpr size_t extra_bytes = Instruction::max_inst_len * 4;
  block->maxsize_ = size(block->insts_) + extra_bytes;
  block->pool_addr_ = block->block_pool_.alloc(block->maxsize_);

  /* write (& relocate) instructions */
  block->write();

  return block;
}

void Block::write(void) {
  uint8_t *addr = pool_addr_;
  addr = block_pool_.write_insts(addr, insts_);
  addr = block_pool_.write_insts(addr, branch_insts_);
  addr = block_pool_.write_insts(addr, fallthrough_insts_);
}

Block::Kind Block::classify_inst(xed_iclass_enum_t iclass, xed_iform_enum_t iform) {
  switch (iclass) {
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
    return Kind::DIR;

  case XED_ICLASS_JMP:
    switch (iform) {
    case XED_IFORM_JMP_GPRv:
    case XED_IFORM_JMP_MEMv:
      return Kind::IND;
    case XED_IFORM_JMP_RELBRb:
    case XED_IFORM_JMP_RELBRd:
      return Kind::DIR;
    default: abort();
    }
    
  case XED_ICLASS_CALL_NEAR:
    switch (iform) {
    case XED_IFORM_CALL_NEAR_GPRv:
    case XED_IFORM_CALL_NEAR_MEMv:
      return Kind::IND;
    case XED_IFORM_CALL_NEAR_RELBRd:
      return Kind::DIR;
    default: abort();
    }

  case XED_ICLASS_RET_NEAR:
    return Kind::IND;

  default:
    return Kind::OTH;
  }
}

uint8_t *BlockPool::alloc(size_t size) {
  assert(alloc_ptr + size <= mem.end<uint8_t>());
  uint8_t *ptr = alloc_ptr;
  alloc_ptr += size;
  return ptr;
}

std::ostream& BlockPool::print(std::ostream& os) const {
  uint8_t *begin = mem.begin<uint8_t>();
  size_t size = alloc_next() - begin;
  uint8_t *buf = (uint8_t *) malloc(size);
  assert(buf != nullptr);
  for (uint8_t *it = buf; it < buf + size; ) {
    Instruction inst(it, tracee);
    os << inst << std::endl;
    it += inst.size();
  }
  return os;
}

uint8_t *BlockPool::write_inst(uint8_t *addr, Blob& inst) {
  inst.relocate(addr);
  tracee.write(inst);
  return addr + inst.size();
}

void Block::jump_to(void) const {
  tracee_.set_pc(pool_addr());
}

void Block::handle_bkpt(uint8_t *pc, const HandleBkptIface& iface) {
  uint8_t bkpt_chk;
  uint8_t bkpt_data;
  tracee_.read(&bkpt_chk, sizeof(bkpt_chk), pc);
  tracee_.read(&bkpt_data, sizeof(bkpt_data), pc + 1);
  const BkptKind bkpt_kind = static_cast<BkptKind>(bkpt_data);

  assert(bkpt_chk == 0xcc);

  switch (bkpt_kind) {
  case BkptKind::BRANCH:
    handle_bkpt_branch(pc, iface);
    break;
  case BkptKind::FALLTHROUGH:
    handle_bkpt_fallthrough(pc, iface);
    break;
  default: abort();
  }
}

void Block::handle_bkpt_branch(uint8_t *pc, const HandleBkptIface& iface) {
  switch (kind()) {
  case Kind::DIR:
    handle_bkpt_branch_dir(pc, iface);
    break;
  case Kind::IND:
    handle_bkpt_branch_ind(pc, iface);
    break;
  default: abort();
  }
}

void Block::handle_bkpt_branch_dir_conditional(uint8_t *pc, const HandleBkptIface& iface) {
  // TODO
  abort();
}

void Block::handle_bkpt_branch_dir(uint8_t *pc, const HandleBkptIface& iface) {
  uint8_t *expected_pc = branch_insts_.front()->pc();
  assert(pc == expected_pc);

  Instruction branch = orig_branch_;

  uint8_t *branch_pool_dst = iface.lb(orig_branch_.branch_dst());

  printf("DIR %p -> %p\n", pc, branch_pool_dst);
  
  branch_insts_.clear();
  
  if (orig_branch_.xed_iclass() == XED_ICLASS_CALL_NEAR) {
    /* push + jmp */
    auto push_it = branch_insts_.insert
      (branch_insts_.end(), std::make_unique<Instruction>(Instruction::push_mem(nullptr, nullptr)));
    auto jmp_it = branch_insts_.insert
      (branch_insts_.end(), std::make_unique<Instruction>(Instruction::jmp_mem(nullptr, nullptr)));

    /* add data */
    uint8_t *after_branch_orig = orig_branch_.after_pc();
    auto push_ptr_it = branch_insts_.insert
      (branch_insts_.end(), std::make_unique<Pointer>(after_branch_orig));
    auto jmp_ptr_it = branch_insts_.insert
      (branch_insts_.end(), std::make_unique<Pointer>(branch_pool_dst));

    printf("CALL: pushing %p\n", after_branch_orig);

    write(); // assign PCs
    
    /* link push + jmp to ptrs */
    (*push_it)->retarget((*push_ptr_it)->pc());
    (*jmp_it)->retarget((*jmp_ptr_it)->pc());
    
  } else {
    branch.retarget(branch_pool_dst);
    assert(branch.branch_dst() == branch_pool_dst);
    branch_insts_.push_back(std::make_unique<Instruction>(branch));
  }

  write();

#if 0
  fprintf(stderr, "orig inst:    %s\n", Decoder::disas(orig_branch_).c_str());
  fprintf(stderr, "decoded inst: %s\n",
	  Decoder::disas(dynamic_cast<Instruction&>(*branch_insts_.front())).c_str());
#endif
}

void Block::handle_bkpt_branch_ind(uint8_t *pc, const HandleBkptIface& iface) {
  if (orig_branch_.xed_iclass() == XED_ICLASS_RET_NEAR) {
    printf("RET\n");
  }

  InstVec saved_branch_insts;
  std::swap(saved_branch_insts, branch_insts_);
  
  branch_insts_.push_back(std::make_unique<Instruction>(orig_branch_));
  write();

  /* single-step thru indirect branch */
  printf("ss addr %p\n", tracee_.get_pc());
  std::clog << "ss inst: " << *branch_insts_.front() << std::endl;
  iface.ss();
  printf("ss addr %p\n", tracee_.get_pc());
  Instruction next_inst(tracee_.get_pc(), tracee_);
  std::clog << "ss inst: " << next_inst << std::endl;

  /* lookup destination block */
  uint8_t *branch_pool_dst = iface.lb(tracee_.get_pc());
  tracee_.set_pc(branch_pool_dst);

  std::swap(saved_branch_insts, branch_insts_);
  write();

  printf("IND %p -> %p\n", pc, branch_pool_dst);
}

void Block::handle_bkpt_fallthrough(uint8_t *pc, const HandleBkptIface& iface) {
  fallthrough_insts_.clear();
  
  /* Get destination */
  uint8_t *orig_dst = orig_branch_.after_pc();
  uint8_t *pool_dst = iface.lb(orig_dst);

  auto jmp_it = fallthrough_insts_.insert
    (fallthrough_insts_.end(), std::make_unique<Instruction>
     (Instruction::jmp_mem(nullptr, nullptr)));

  constexpr size_t dstptr_size = sizeof(&pool_dst);
  Data::Content dstptr(dstptr_size);
  std::memcpy(dstptr.data(), reinterpret_cast<uint8_t *>(&pool_dst), dstptr_size);
  auto mem_it = fallthrough_insts_.insert(fallthrough_insts_.end(), std::make_unique<Data>(dstptr));
  
  write(); // TODO: This is inefficient -- should directly compute address instead.

  /* link JMP to mem ptr */
  (*jmp_it)->retarget((*mem_it)->pc());

  write(); // TODO: Inefficient -- should just copy one instruction.
}

const char *Block::bkpt_kind_to_str(BkptKind bkpt_kind) {
  switch (bkpt_kind) {
  case BkptKind::BRANCH: return "BRANCH";
  case BkptKind::FALLTHROUGH: return "FALLTHROUGH";
  default: return nullptr;
  }
}

const char *Block::kind_to_str(Kind kind) {
  switch (kind) {
  case Kind::DIR: return "DIR";
  case Kind::IND: return "IND";
  default: return nullptr;
  }
}

bool Block::may_have_conditional_branch(void) const {
  const auto iclass = orig_branch_.xed_iclass();
  if (iclass == XED_ICLASS_CALL_NEAR ||
      iclass == XED_ICLASS_JMP ||
      iclass == XED_ICLASS_RET_NEAR) {
    return false;
  } else {
    return true;
  }
}

#if 0
bool Block::transfom_call_to_jmp(uint8_t *pc, InstVec::iterator branch_it) {
  if (!(*branch_it)->call_to_jmp()) {
    return false; // original branch wasn't a call
  }

  uint8_t *orig_fallthru = orig_branch_.after_pc();

  auto push_mem_it =
    branch_insts_.insert(std::next(branch_it, 1), std::make_unique<Pointer>(orig_fallthru));

  // branch_insts_.insert
  
  


  Instruction push = Instruction::push_mem(pc, nullptr);
  
  // *it++ = std::make_unique<Instruction>(Instruction::push_mem(pc, 
  
  *it++ = std::make_unique<Pointer>(orig_fallthru);
  
  Instruction::call_to_jump;
  
  // TODO
  abort();
}
#endif
