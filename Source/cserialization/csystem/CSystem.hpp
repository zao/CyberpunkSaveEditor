#pragma once
#include <iostream>
#include <list>
#include <array>
#include <exception>

#include <utils.hpp>
#include <cpinternals/cpnames.hpp>
#include <cserialization/node.hpp>
#include <cserialization/serializers.hpp>
#include <cserialization/csystem/CStringPool.hpp>
#include <cserialization/csystem/CObject.hpp>

enum class ECSystemKind : uint8_t
{
  DropPointSystem,
  GameplaySettingsSystem,
  EquipmentSystem,
  PlayerDevelopmentSystem,
  BraindanceSystem,
  UIScriptableSystem,
  PreventionSystem,
  FocusCluesSystem,
  FastTravelSystem,
  CityLightSystem,
  FirstEquipSystem,
  SubCharacterSystem,
  SecSystemDebugger,
  MarketSystem,
  CraftingSystem,
  DataTrackingSystem,
};

/*
RenderGameplayEffectsManagerSystem, 
PSData,
godModeSystem,
MovingPlatformSystem,
scanningController,
ScriptableSystemsContainer,
StatPoolsSystem,
StatsSystem,
tierSystem,
(maybeCharacetrCustomization_Appearances,
but the function is weird to read, there is also CharacterCustomizationSystem_Components_v2 in it, maybe old stuff),
(staticshader_final.cache, ShaderCacheReadOnly)
*/

struct CSystemObject
{
  CSystemObject() = default;

  CSystemObject(std::string_view name, const CObjectSPtr& obj)
    : name(name), obj(obj) {}

  std::string name;
  CObjectSPtr obj;
};

class CSystem
{
private:
  struct header_t
  {
    uint16_t uk1                  = 0;
    uint16_t uk2                  = 0;
    uint32_t cnames_cnt           = 0;
    uint32_t uk3                  = 0;
    uint32_t strpool_data_offset  = 0;
    uint32_t obj_descs_offset     = 0;
    // tricky: obj offsets are relative to the strpool base
    uint32_t objdata_offset       = 0; 
  };

  struct obj_desc_t
  {
    obj_desc_t() = default;

    obj_desc_t(uint32_t name_idx, uint32_t data_offset)
      : name_idx(name_idx), data_offset(data_offset) {}

    uint32_t name_idx     = 0;
    uint32_t data_offset  = 0;// relative to the end of the header in stream
  };

  header_t m_header;
  std::vector<CName> m_subsys_names;
  std::vector<CSystemObject> m_objects;

public:
  CSystem() = default;


  const std::vector<CName>& subsys_names() const { return m_subsys_names; }
        std::vector<CName>& subsys_names()       { return m_subsys_names; }

  const std::vector<CSystemObject>& objects() const { return m_objects; }
        std::vector<CSystemObject>& objects()       { return m_objects; }

  // for now let's not rebuild it from scratch since we don't handle all types..
  CStringPool m_strpool;

  bool serialize_in(std::istream& reader)
  {
    m_subsys_names.clear();
    m_objects.clear();

    uint32_t blob_size;
    reader >> cbytes_ref(blob_size);

    // let's get our header start position
    auto blob_spos = reader.tellg();

    reader >> cbytes_ref(m_header);

    // check header
    if (m_header.obj_descs_offset < m_header.strpool_data_offset)
      return false;
    if (m_header.objdata_offset < m_header.obj_descs_offset)
      return false;

    m_subsys_names.clear();
    if (m_header.cnames_cnt > 1)
    {
      uint32_t cnames_cnt = 0;
      reader >> cbytes_ref(cnames_cnt);
      
      if (cnames_cnt != m_header.cnames_cnt)
        return false;

      m_subsys_names.resize(cnames_cnt);
      reader.read((char*)m_subsys_names.data(), m_header.cnames_cnt * sizeof(CName));
    }

    // end of header
    const size_t base_offset = (reader.tellg() - blob_spos);
    if (base_offset + m_header.objdata_offset > blob_size)
      return false;

    // first let's read the string pool

    const uint32_t strpool_descs_size = m_header.strpool_data_offset;
    const uint32_t strpool_data_size = m_header.obj_descs_offset - strpool_descs_size;

    //CStringPool strpool;
    CStringPool& strpool = m_strpool;
    if (!strpool.serialize_in(reader, strpool_descs_size, strpool_data_size))
      return false;

    // now let's read objects

    // we don't have the impl for all props
    // so we'll use the assumption that everything is serialized in
    // order and use the offset of next item as end of blob

    const size_t obj_descs_offset = m_header.obj_descs_offset;
    const size_t obj_descs_size = m_header.objdata_offset - obj_descs_offset;
    if (obj_descs_size % sizeof(obj_desc_t) != 0)
      return false;

    const size_t obj_descs_cnt = obj_descs_size / sizeof(obj_desc_t);
    if (obj_descs_cnt == 0)
      return m_header.objdata_offset + base_offset == blob_size; // could be empty

    std::vector<obj_desc_t> obj_descs(obj_descs_cnt);
    reader.read((char*)obj_descs.data(), obj_descs_size);

    // read objdata
    const size_t objdata_size = blob_size - (base_offset + m_header.objdata_offset); 

    if (base_offset + m_header.objdata_offset != (reader.tellg() - blob_spos))
      return false;

    std::vector<char> objdata(objdata_size);
    reader.read((char*)objdata.data(), objdata_size);

    if (blob_size != (reader.tellg() - blob_spos))
      return false;

    // here the offsets relative to base_offset are converted to offsets relative to objdata
    
    size_t next_obj_offset = objdata_size;
    for (auto it = obj_descs.rbegin(); it != obj_descs.rend(); ++it)
    {
      const auto& desc = *it;
      auto obj_name = strpool.from_idx(desc.name_idx);

      auto new_obj = std::make_shared<CObject>();

      if (desc.data_offset < m_header.objdata_offset)
        return false;

      const size_t offset = desc.data_offset - m_header.objdata_offset;
      if (offset > next_obj_offset)
        throw std::logic_error("CSystem: false assumption #2. please open an issue.");

      std::span<char> objblob((char*)objdata.data() + offset, next_obj_offset - offset);
      if (!new_obj->serialize_in(objblob, strpool))
        return false;

      m_objects.emplace_back(obj_name, new_obj);

      next_obj_offset = offset;
    }
    std::reverse(m_objects.begin(), m_objects.end());

    return true;
  }

  bool serialize_out(std::ostream& writer) const
  {
    // let's not do too many copies

    auto start_spos = writer.tellp();
    uint32_t blob_size = 0; // we don't know it yet
    
    writer << cbytes_ref(blob_size); 
    writer << cbytes_ref(m_header); 

    header_t new_header = m_header;

    const uint32_t cnames_cnt = (uint32_t)m_subsys_names.size();
    new_header.cnames_cnt = 1;
    if (cnames_cnt)
    {
      new_header.cnames_cnt = cnames_cnt;
      writer << cbytes_ref(cnames_cnt);
      writer.write((char*)m_subsys_names.data(), cnames_cnt * sizeof(CName));
    }

    auto base_spos = writer.tellp();

    // i see no choice but to build objects first to populate the pool
    // the game probably actually uses this pool so they don't have
    // to do that, but it is also not leaned at all.

    //CStringPool strpool;
    CStringPool& strpool = const_cast<CStringPool&>(m_strpool);
    std::ostringstream ss;
    std::vector<obj_desc_t> obj_descs;
    obj_descs.reserve(m_objects.size());

    for (auto& obj : m_objects)
    {
      const uint32_t tmp_offset = (uint32_t)ss.tellp();
      obj_descs.emplace_back(strpool.to_idx(obj.name), tmp_offset);
      if (!obj.obj->serialize_out(ss, strpool))
        return false;
    }

    // time to write strpool
    uint32_t strpool_pool_size = 0;
    if (!strpool.serialize_out(writer, new_header.strpool_data_offset, strpool_pool_size))
      return false;

    new_header.obj_descs_offset = new_header.strpool_data_offset + strpool_pool_size;

    // reoffset offsets
    const uint32_t obj_descs_size = (uint32_t)(obj_descs.size() * sizeof(obj_desc_t));
    const uint32_t objdata_offset = new_header.obj_descs_offset + obj_descs_size;

    new_header.objdata_offset = objdata_offset;

    // reoffset offsets
    for (auto& desc : obj_descs)
      desc.data_offset += objdata_offset;

    // write obj descs + data
    writer.write((char*)obj_descs.data(), obj_descs_size);
    auto objdata = ss.str(); // not optimal but didn't want to depend on boost and no time to dev a mem stream for now
    writer.write(objdata.data(), objdata.size());
    auto end_spos = writer.tellp();

    // at this point data should be correct except blob_size and header
    // so let's rewrite them

    writer.seekp(start_spos);
    blob_size = (uint32_t)(end_spos - start_spos - 4);
    writer << cbytes_ref(blob_size); 
    writer << cbytes_ref(new_header); 

    // return to end of data
    writer.seekp(end_spos);

    return true;
  }

  friend std::istream& operator>>(std::istream& reader, CSystem& sys)
  {
    if (!sys.serialize_in(reader))
      reader.setstate(std::ios_base::badbit);
    return reader;
  }

  friend std::ostream& operator<<(std::ostream& writer, CSystem& sys)
  {
    sys.serialize_out(writer);
    return writer;
  }
};


