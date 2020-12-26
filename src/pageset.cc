#include "pageset.hh"

void PageInfo::recompute_tier() {
  if ((flags_ & MAP_SHARED)) {
    assert(cur_prot_ == PROT_NONE);
    tier_ = Tier::SHARED;
  } else if (!(orig_prot_ & PROT_WRITE)) {
    /* NOTE: This includes a page with any of the following permissions:
     *  PROT_NONE
     *  PROT_READ
     *  PROT_READ | PROT_EXEC
     */
    assert(orig_prot_ == cur_prot_);
    tier_ = Tier::RDONLY;
  } else if ((cur_prot_ & PROT_WRITE)) {
    assert(orig_prot_ == cur_prot_);
    tier_ = Tier::RDWR_UNLOCKED;
  } else {
    assert(cur_prot_ == (orig_prot_ & ~PROT_WRITE));
    tier_ = Tier::RDWR_LOCKED;
  }
}

void PageSet::add_maps(Maps& maps_gen) {
  std::vector<::Map> tmp_maps;
  maps_gen.get_maps(std::back_inserter(tmp_maps));
  std::for_each(tmp_maps.begin(), tmp_maps.end(), [&] (const auto& map) {
    PageInfo page_info{map.flags, map.prot, map.prot};
    for_each_page(map.begin, map.end, [this, &page_info] (const auto pageaddr) {
      this->track_page(pageaddr, page_info);
    });
  });
}

void PageSet::untrack_page(void *pageaddr) {
  map.erase(pageaddr);
}

void PageSet::untrack_range(void *begin, void *end) {
  for_each_page(begin, end, [this] (void *pageaddr) { untrack_page(pageaddr); });    
}

void PageInfo::lock(void *pageaddr, Tracee& tracee, int mask) {
  if (g_conf.verbosity >= 1) {
    *g_conf.log << "LOCKING PAGE " << (void *) pageaddr << "\n";
  }
  
  assert(tier() == Tier::RDWR_UNLOCKED);
  
  assert(orig_prot_ == cur_prot_);
  assert((orig_prot_ & mask) == mask);
  tracee.syscall(Syscall::MPROTECT, (uintptr_t) pageaddr, PAGESIZE, cur_prot_);

  prot(orig_prot_, orig_prot_ & ~mask);
  
  assert(tier() == Tier::RDWR_LOCKED);
}

void PageInfo::unlock(void *pageaddr, Tracee& tracee) {
  if (g_conf.verbosity >= 1) {
    *g_conf.log << "UNLOCKING PAGE " << (void *) pageaddr << "\n";
  }
  
  assert(tier() == Tier::RDWR_LOCKED);
  
  tracee.syscall(Syscall::MPROTECT, (uintptr_t) pageaddr, PAGESIZE, orig_prot_);
  ++count_;
  prot(orig_prot_, orig_prot_);
  
  assert(tier() == Tier::RDWR_UNLOCKED);
}

void PageSet::lock_top_counts(unsigned n, Tracee& tracee, int mask) {
  /* get map of counts to page map iterators */
  std::multimap<unsigned, Map::iterator> counts_map;

  for (auto it = begin(); it != end(); ++it) {
    switch (it->second.tier()) {
    case PageInfo::Tier::RDWR_LOCKED:
    case PageInfo::Tier::RDWR_UNLOCKED:
      counts_map.emplace(it->second.count(), it);
      break;
    }
  }

  *g_conf.log << "count: " << counts_map.size() << "\n";
  
  /* find top N */
  auto rit = counts_map.rbegin();
  for (unsigned i = 0; i < n && rit != counts_map.rend(); ++i, ++rit) {
    const auto pageaddr = rit->second->first;
    PageInfo& page_info = rit->second->second;
    if (page_info.tier() == PageInfo::Tier::RDWR_LOCKED) {
      page_info.unlock(pageaddr, tracee);
    }
  }
  for (; rit != counts_map.rend(); ++rit) {
    const auto pageaddr = rit->second->first;
    PageInfo& page_info = rit->second->second;
    if (page_info.tier() == PageInfo::Tier::RDWR_UNLOCKED) {
      page_info.lock(pageaddr, tracee, mask);
    }
  }
}
