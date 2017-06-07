#include <steemit/chain/database_exceptions.hpp>

#include <steemit/chain_plugin/chain_plugin.hpp>

#include <fc/io/json.hpp>

namespace steemit { namespace chain_plugin {

using namespace steemit;
using fc::flat_map;
using chain::block_id_type;

class chain_plugin_impl {
   public:
      uint64_t                         shared_memory_size = 0;
      bfs::path                        shared_memory_dir;
      bool                             replay = false;
      bool                             reset   = false;
      bool                             readonly = false;
      uint32_t                         flush_interval = 0;
      flat_map<uint32_t,block_id_type> loaded_checkpoints;

      database  db;
};


chain_plugin::chain_plugin()
:my( new chain_plugin_impl() ) {
}

chain_plugin::~chain_plugin(){}

database& chain_plugin::db() { return my->db; }
const chain::database& chain_plugin::db() const { return my->db; }

void chain_plugin::set_program_options(options_description& cli, options_description& cfg)
{
   cfg.add_options()
         ("readonly", bpo::value<bool>()->default_value(false), "open the database in read only mode")
         ("shared-file-dir", bpo::value<bfs::path>()->default_value("blockchain"),
            "the location of the chain shared memory files (absolute path or relative to application data dir)")
         ("shared-file-size", bpo::value<uint64_t>()->default_value(8*1024),
            "Minimum size MB of database shared memory file")
         ("checkpoint,c", bpo::value<vector<string>>()->composing(), "Pairs of [BLOCK_NUM,BLOCK_ID] that should be enforced as checkpoints.")
         ("flush-state-interval", bpo::value<uint32_t>()->default_value(0),
          "flush shared memory changes to disk every N blocks")
         ;
   cli.add_options()
         ("replay-blockchain", bpo::bool_switch()->default_value(false),
          "clear chain database and replay all blocks")
         ("resync-blockchain", bpo::bool_switch()->default_value(false),
          "clear chain database and block log")
         ;
}

void chain_plugin::plugin_initialize(const variables_map& options) {
   ilog("initializing chain plugin");
   my->shared_memory_dir = app().data_dir() / "blockchain";

   if(options.count("shared-file-dir")) {
      auto sfd = options.at("shared-file-dir").as<bfs::path>();
      if(sfd.is_relative())
         my->shared_memory_dir = app().data_dir() / sfd;
      else
         my->shared_memory_dir = sfd;
   }
   my->shared_memory_size = options.at("shared-file-size").as<uint64_t>() * 1024 * 1024;
   my->readonly = options.at("readonly").as<bool>();
   my->replay   = options.at("replay-blockchain").as<bool>();
   my->reset    = options.at("resync-blockchain").as<bool>();
   my->flush_interval = options.at("flush-state-interval").as<uint32_t>();

   if(options.count("checkpoint"))
   {
      auto cps = options.at("checkpoint").as<vector<string>>();
      my->loaded_checkpoints.reserve(cps.size());
      for(auto cp : cps)
      {
         auto item = fc::json::from_string(cp).as<std::pair<uint32_t,block_id_type>>();
         my->loaded_checkpoints[item.first] = item.second;
      }
   }
}

void chain_plugin::plugin_startup()
{
   if(!my->readonly)
   {
      ilog("starting chain in read/write mode");

      if(my->reset)
      {
         wlog("resync requested: deleting block log and shared memory");
         my->db.wipe( app().data_dir() / "blockchain", my->shared_memory_dir, true );
      }

      my->db.set_flush_interval(my->flush_interval);
      my->db.add_checkpoints(my->loaded_checkpoints);

      if(my->replay)
      {
         ilog("Replaying blockchain on user request.");
         my->db.reindex( app().data_dir() / "blockchain", my->shared_memory_dir, my->shared_memory_size );
      }
      else
      {
         try
         {
            ilog("Opening shared memory from ${path}", ("path",my->shared_memory_dir.generic_string()));
            my->db.open( app().data_dir() / "blockchain", my->shared_memory_dir, 0, my->shared_memory_size, chainbase::database::read_write );
         }
         catch( const fc::exception& e )
         {
            wlog("Error opening database, attempting to replay blockchain. Error: ${e}", ("e", e));

            try
            {
               my->db.reindex( app().data_dir() / "blockchain", my->shared_memory_dir, my->shared_memory_size );
            }
            catch( chain::block_log_exception& )
            {
               wlog( "Error opening block log. Having to resync from network..." );
               my->db.open( app().data_dir() / "blockchain", my->shared_memory_dir, 0, my->shared_memory_size, chainbase::database::read_write );
            }
         }
      }
   }
   else
   {
      ilog("Starting chain in read mode.");
      my->db.open( app().data_dir() / "blockchain", my->shared_memory_dir, my->shared_memory_size, chainbase::database::read_only );
   }
}

void chain_plugin::plugin_shutdown() {
   ilog("closing chain database");
   my->db.close();
   ilog("database closed successfully");
}

bool chain_plugin::accept_block(const chain::signed_block& block, bool currently_syncing) {
   if (currently_syncing && block.block_num() % 10000 == 0) {
      ilog("Syncing Blockchain --- Got block: #${n} time: ${t} producer: ${p}",
           ("t", block.timestamp)
           ("n", block.block_num()) );
   }

   return db().push_block(block);
}

void chain_plugin::accept_transaction(const chain::signed_transaction& trx) {
   db().push_transaction(trx);
}

bool chain_plugin::block_is_on_preferred_chain(const chain::block_id_type& block_id) {
   // If it's not known, it's not preferred.
   if (!db().is_known_block(block_id)) return false;
   // Extract the block number from block_id, and fetch that block number's ID from the database.
   // If the database's block ID matches block_id, then block_id is on the preferred chain. Otherwise, it's on a fork.
   return db().get_block_id_for_num(chain::block_header::num_from_id(block_id)) == block_id;
}

namespace chain_apis {

read_only::get_info_results read_only::get_info(const read_only::get_info_params&) const {
   return {
      db.head_block_num(),
      db.head_block_id(),
      db.head_block_time(),
      db.get_dynamic_global_properties().current_witness,
      db.get_dynamic_global_properties().recent_slots_filled,
      db.get_dynamic_global_properties().participation_count / 128.0
   };
}

read_only::get_block_results read_only::get_block(const read_only::get_block_params& params) const {
   read_only::get_block_results block;

   try
   {
      block = db.fetch_block_by_id(fc::json::from_string(params.block_num_or_id).as<chain::block_id_type>());
   }
   catch (fc::bad_cast_exception) {/* do nothing */}

   try
   {
      block = db.fetch_block_by_number(fc::to_uint64(params.block_num_or_id));
   }
   catch (fc::bad_cast_exception) {/* do nothing */}

   return block;
}

read_write::push_block_results read_write::push_block(const read_write::push_block_params& params) {
   db.push_block(params);
   return read_write::push_block_results();
}

read_write::push_transaction_results read_write::push_transaction(const read_write::push_transaction_params& params) {
   db.push_transaction(params);
   return read_write::push_transaction_results();
}

} } }// namespace steemit::chain_plugin::chain_apis
