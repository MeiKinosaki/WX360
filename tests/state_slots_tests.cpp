#include "onix360/state/state_slots.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>
namespace {int failures=0;void check(bool v,const char*m){if(!v){std::cerr<<"FAIL: "<<m<<'\n';++failures;}}}
int main(){
 using namespace onix360::state;
 const std::filesystem::path dir="onix360_slots_test";
 std::error_code ec;std::filesystem::remove_all(dir,ec);
 StateSlots slots(dir);std::string error;Snapshot first;first.cpu.pc=0x1111;first.ram={1,2,3};
 Snapshot second;second.cpu.pc=0x2222;second.ram={4,5,6,7};
 check(!slots.exists(0),"new slot starts empty");
 check(slots.save(0,first,&error),"saves slot zero");
 check(slots.save(1,second,&error),"saves slot one");
 check(slots.exists(0)&&slots.exists(1),"lists occupied slots");
 check(slots.occupied_slots()==std::vector<unsigned>{0U,1U},
       "enumerates occupied save-state slots in numeric order");
 check(slots.available_slots().size()==8U&&slots.available_slots().front()==2U,
       "enumerates the eight available save-state slots");
 check(slots.path_for(0).extension()==".onix360",
       "uses the Onix360 save-state file extension");
 Snapshot loaded;check(slots.load(0,loaded,&error),"loads slot zero");
 check(loaded.cpu.pc==first.cpu.pc&&loaded.ram==first.ram,"slot zero restores its own snapshot");
 check(slots.load(1,loaded,&error),"loads slot one");
 check(loaded.cpu.pc==second.cpu.pc&&loaded.ram==second.ram,"slot one restores independently");
 check(!slots.save(StateSlots::kSlotCount,first,&error),"rejects out-of-range save slot");
 check(!slots.load(StateSlots::kSlotCount,loaded,&error),"rejects out-of-range load slot");
 check(slots.erase(0,&error)&&!slots.exists(0),"deletes selected slot");
 check(slots.exists(1),"deleting one slot preserves another");
 std::filesystem::remove_all(dir,ec);return failures?EXIT_FAILURE:EXIT_SUCCESS;
}