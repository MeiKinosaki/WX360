#pragma once
#include "onix360/state/state_file.hpp"
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
namespace onix360::state {
class StateSlots {
public:
 static constexpr unsigned kSlotCount=10;
 explicit StateSlots(std::filesystem::path directory):directory_(std::move(directory)){}
 static bool valid_slot(unsigned slot) noexcept{return slot<kSlotCount;}
 std::filesystem::path path_for(unsigned slot)const{return directory_/("slot_"+std::to_string(slot)+".onix360");}
 std::vector<unsigned> occupied_slots()const{
  std::vector<unsigned> result;
  for(unsigned slot=0;slot<kSlotCount;++slot)if(exists(slot))result.push_back(slot);
  return result;
 }
 std::vector<unsigned> available_slots()const{
  std::vector<unsigned> result;
  for(unsigned slot=0;slot<kSlotCount;++slot)if(!exists(slot))result.push_back(slot);
  return result;
 }
 bool exists(unsigned slot)const{return valid_slot(slot)&&std::filesystem::is_regular_file(path_for(slot));}
 bool save(unsigned slot,const Snapshot& snapshot,std::string* error=nullptr)const{
  if(!valid_slot(slot))return fail(error,"save-state slot is out of range");
  std::error_code ec;std::filesystem::create_directories(directory_,ec);
  if(ec)return fail(error,"cannot create save-state directory");
  auto target=path_for(slot);auto temporary=target;temporary += ".tmp";
  if(!StateFile::save(temporary.string(),snapshot,error))return false;
  std::filesystem::rename(temporary,target,ec);
  if(ec){
   std::error_code remove_error;std::filesystem::remove(target,remove_error);ec.clear();
   std::filesystem::rename(temporary,target,ec);
  }
  if(ec){std::error_code ignored;std::filesystem::remove(temporary,ignored);return fail(error,"cannot finalize save-state slot");}
  if(error)error->clear();return true;
 }
 bool load(unsigned slot,Snapshot& destination,std::string* error=nullptr)const{
  if(!valid_slot(slot))return fail(error,"save-state slot is out of range");
  if(!exists(slot))return fail(error,"save-state slot is empty");
  return StateFile::load(path_for(slot).string(),destination,error);
 }
 bool erase(unsigned slot,std::string* error=nullptr)const{
  if(!valid_slot(slot))return fail(error,"save-state slot is out of range");
  std::error_code ec;bool removed=std::filesystem::remove(path_for(slot),ec);
  if(ec)return fail(error,"cannot delete save-state slot");
  if(!removed)return fail(error,"save-state slot is empty");
  if(error)error->clear();return true;
 }
private:
 static bool fail(std::string* error,const char* message){if(error)*error=message;return false;}
 std::filesystem::path directory_;
};
}