#include <iostream>
#include <vector>
#include <thread>
#include <chrono> // for time measurement

#include <steem/chain/steem_object_types.hpp> // include at first because of needed specializations for pack and unpack
#include <steem/chain/database.hpp>
#include <steem/chain/comment_object.hpp>
#include <steem/chain/account_object.hpp>
#include <steem/chain/witness_objects.hpp>
#include <steem/chain/steem_objects.hpp>

#include <steem/protocol/types.hpp>

#include <steem/plugins/database_api/database_api_objects.hpp>
#include <steem/plugins/transaction_status/transaction_status_objects.hpp>
#include <steem/plugins/market_history/market_history_plugin.hpp>
#include <steem/plugins/reputation/reputation_objects.hpp>
#include <steem/plugins/account_by_key/account_by_key_objects.hpp>
#include <steem/plugins/witness/witness_plugin_objects.hpp>
#include <steem/plugins/account_history_rocksdb/account_history_rocksdb_objects.hpp>
#include <steem/plugins/rc/rc_objects.hpp>
// #include <steem/chain/pending_optional_action_object.hpp> // nothing happens there
// #include <steem/chain/pending_required_action_object.hpp> // nothing happens there

#define CONSOLE_BOLD_RED    "\033[1;31m"
#define CONSOLE_RESET       "\033[0m"

using IndexSize = std::function<void( steem::chain::database&, int& )>;
using IndexComparator = std::function<void( steem::chain::database&, steem::chain::database&, const fc::string&, int&, int& )>;
struct IndexOps
{
    IndexSize       sizeHandler;
    IndexComparator comparator;
};
using IndexOpsMap = std::map<fc::string, IndexOps>;

static void get_different_indices( const std::vector<fc::string>& db1_indices, const std::vector<fc::string>& db2_indices, std::vector<fc::string>& in_db1_not_in_db2, std::vector<fc::string>& in_db2_not_in_db1 )
{
    for ( const auto& idx : db1_indices )
    {
        if ( std::find( db2_indices.begin(), db2_indices.end(), idx ) == db2_indices.end() )
            in_db1_not_in_db2.push_back( idx );
    }
    for ( const auto& idx : db2_indices )
    {
        if ( std::find( db1_indices.begin(), db1_indices.end(), idx ) == db1_indices.end() )
            in_db2_not_in_db1.push_back( idx );
    }
}

static void print_hex_diff( const std::vector<char>& a, const std::vector<char>& b )
{
    const std::size_t cols = 16;
    const std::size_t maxlen = std::max(a.size(), b.size());
    for ( std::size_t off = 0; off < maxlen; off += cols )
    {
        // offset
        std::cout << std::hex << std::setfill('0') << std::setw(8) << off << std::dec << ": ";
        // hex columns
        for ( std::size_t i = 0; i < cols; ++i )
        {
            const std::size_t idx = off + i;
            if ( idx < maxlen )
            {
                const bool diff = ( idx >= a.size() ) || ( idx >= b.size() ) || ( a[idx] != b[idx] );
                if ( diff ) 
                    std::cout << CONSOLE_BOLD_RED;
                if ( idx < a.size() )
                    std::cout << std::hex << std::setw(2) << (static_cast<int>(static_cast<unsigned char>(a[idx])) & 0xff);
                else
                    std::cout << "  ";
                if ( diff ) 
                    std::cout << CONSOLE_RESET;
                std::cout << " ";
            }
            else
            {
                std::cout << "   ";
            }
            if ( i == 7 ) 
                std::cout << " ";
        }
        // ascii column (from a)
        std::cout << " | ";
        for ( std::size_t i = 0; i < cols; ++i )
        {
            const std::size_t idx = off + i;
            if ( idx < maxlen )
            {
                const bool diff = ( idx >= a.size() ) || ( idx >= b.size() ) || ( a[idx] != b[idx] );
                const char ch = ( idx < a.size() ? a[idx] : '.' );
                const char outc = std::isprint(static_cast<unsigned char>(ch)) ? ch : '.';
                if ( diff ) 
                    std::cout << CONSOLE_BOLD_RED << outc << CONSOLE_RESET;
                else std::cout << outc;
            }
            else
            {
                std::cout << ' ';
            }
        }
        std::cout << std::endl;
    }
}

static void print_index_numbers( const std::vector<fc::string>& db1_data_indices, const std::vector<fc::string>& db2_data_indices )
{
    const auto difference = (db1_data_indices.size() > db2_data_indices.size())
        ? (db1_data_indices.size() - db2_data_indices.size())
        : (db2_data_indices.size() - db1_data_indices.size());
    std::cout << "-------------------------------------------------------" << std::endl;
    std::cout << "Number of data indices" << std::endl;
    std::cout << "DB1: " << db1_data_indices.size() << " | DB2: " << db2_data_indices.size();
    
    if (difference > 0)
    {
        std::cout << CONSOLE_BOLD_RED << " (" << difference << ")" << CONSOLE_RESET << std::endl;
        std::vector<fc::string> in_db1_not_in_db2;
        std::vector<fc::string> in_db2_not_in_db1;
        get_different_indices( db1_data_indices, db2_data_indices, in_db1_not_in_db2, in_db2_not_in_db1 );
        std::cout << "Indices in DB1 not in DB2:" << std::endl;
        for ( const auto& idx : in_db1_not_in_db2 )
        {
            std::cout << " - " << idx << std::endl;
        }
        std::cout << "Indices in DB2 not in DB1:" << std::endl;
        for ( const auto& idx : in_db2_not_in_db1 )
        {
            std::cout << " - " << idx << std::endl;
        }
    }
    else
    {
        std::cout << std::endl;
    }
}

static void print_index_sizes( const fc::string& index_name, const int& size1, const int& size2 )
{
    const bool different_sizes = (size1 != size2);
    // lamda function to format string with fixed width
    auto format_string = [](const fc::string& str, const std::size_t& width) {
        if (str.length() >= width)
            return str;
        return fc::string(str + fc::string(width - str.length(), ' ')); 
    };
    std::cout << std::endl << fc::string(index_name) << ":" << std::endl;
    std::cout << format_string(fc::string( "--- Size: DB1: " + (size1 < 0 ? "-" : std::to_string(size1) ) ), 26U);
    std::cout << format_string(fc::string( "| DB2: " + (size2 < 0 ? "-" : std::to_string(size2) ) ), 17U);
    if (different_sizes)
        std::cout << CONSOLE_BOLD_RED << " (" << std::dec << std::abs(size1 - size2) << ")" << CONSOLE_RESET << std::endl;
    else
        std::cout << std::endl;
}

static void print_index_compare_results( const int& equal, const int& diff )
{
    std::cout << fc::string("--- Comparison result: equal: " + (equal < 0 ? "-" : std::to_string(equal)));
    std::cout << fc::string(", diff: " + (diff < 0 ? "-" : std::to_string(diff)));
    std::cout << std::endl;
}

// search for subdirectories in a given directory
// returns vector of subdirectory names
static std::vector<fc::string> get_indices_from_subdirs(const fc::path& dir)
{
    std::vector<fc::string> indices;
    fc::directory_iterator end_itr;
    for (fc::directory_iterator itr(dir); itr != end_itr; ++itr)
    {
        if (fc::is_directory(*itr))
        {
            // remove rocksdb_ prefix from directory name and replay 'object' with 'index'
            const fc::string subdir = itr->filename().string();
            if (subdir.find("rocksdb_") == 0)
            {
                fc::string subdir_name = itr->filename().string().substr(8); 
                subdir_name.replace(subdir_name.find("object"), 6, "index");
                indices.push_back(subdir_name);
            }
        }
    }
    return indices;
}

template <typename It>
static void compare_slice(const It it1_first, const It it1_last, const It it2_first, const It it2_last, const fc::string& index_name, int& equal, int& diff)
{
    auto it1 = it1_first;
    auto it2 = it2_first;
    equal = 0;
    diff = 0;
    
    // std::cout << "slice size: " << std::distance(it1_first, it1_last) << ", first ids: " << it1->id._id << ", " << it2->id._id << std::endl;

    while (true)
    {
        const bool end1 = (it1 == it1_last);
        const bool end2 = (it2 == it2_last);
        if (end1 || end2) 
            break;

        // db objects have an id._id member
        const auto id1 = it1->id._id;
        const auto id2 = it2->id._id;

        if (id1 == id2)
        {
            std::vector<char> b1;
            std::vector<char> b2;
            try 
            {
                b1 = fc::raw::pack_to_vector( *it1 );
                b2 = fc::raw::pack_to_vector( *it2 );
            } 
            catch (...) 
            {
                std::cerr << "Error packing objects in " << index_name << " with id " << id1 << std::endl;
            }
            if (b1 == b2) 
            {
                ++equal;
            } 
            else 
            {
                try 
                {
                    if (diff < 5)
                    {
                        std::cout << "--- Difference in " << index_name << ": id = " << id1 << " ---" << std::endl;
                        print_hex_diff( b1, b2 );
                    }
                }
                catch(...) {}
                ++diff;
            }
        }
        else
        {
            ++diff;
        }
        ++it1;
        ++it2;
    }

}

template <typename Index>
static void compare_indices(const Index& idx1, const Index& idx2, const fc::string& index_name, int& equal, int& diff)
{
    constexpr std::uint32_t min_per_thread = 500'000;
    const auto size1 = idx1.size();
    const auto size2 = idx2.size();

    const auto size = std::min<std::uint32_t>( size1, size2 );
    if ( size == 0 )
        return;

    if ( size < min_per_thread * 2 )
    {
        compare_slice( idx1.begin(), idx1.end(), idx2.begin(), idx2.end(), index_name, equal, diff );
        return;
    }

    const std::uint32_t hw = std::thread::hardware_concurrency();
    const std::uint32_t num_cpus = (hw > 0) ? hw : 1;

    // every thread should process at least min_per_thread elements
    const std::uint32_t max_threads = size / min_per_thread;
    const std::uint32_t num_threads = std::min( num_cpus / 2, max_threads );

    std::vector<int> local_equals(num_threads, 0);
    std::vector<int> local_diffs(num_threads, 0);
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    const auto slice_size = size / num_threads;
    auto it1_prev_last = idx1.begin();
    auto it2_prev_last = idx2.begin();

    for ( std::uint32_t t = 0; t < num_threads; ++t )
    {
        // first iterators of slice
        auto it1_first = it1_prev_last;
        auto it2_first = it2_prev_last;
        auto it1_last = it1_prev_last; // declared here, defined in if/else scope
        auto it2_last = it2_prev_last; // declared here, defined in if/else scope

        if ( t == num_threads - 1 )
        {
            // the last thread takes the rest
            it1_last = idx1.end();
            it2_last = idx2.end();
        }
        else
        {
            // last iterators of slice: slice_size ahead of first
            // iterator_adapter does not support + operator directly
            for ( std::size_t i = 0; i < slice_size; ++i )
            {
                // no end check needed, because size is known
                ++it1_last;
                ++it2_last;
            }
        }

        threads.emplace_back( []( auto it1_first_, 
                                  auto it1_last_, 
                                  auto it2_first_, 
                                  auto it2_last_, 
                                  const fc::string& index_name_, 
                                  int& local_equal, 
                                  int& local_diff) 
            {
                compare_slice( it1_first_, it1_last_, it2_first_, it2_last_, index_name_, local_equal, local_diff );
            },
            it1_first, 
            it1_last, 
            it2_first, 
            it2_last, 
            index_name, 
            std::ref(local_equals[t]), 
            std::ref(local_diffs[t])
        );
        
        // the next previous last iterators is the current last iterators
        it1_prev_last = it1_last;
        it2_prev_last = it2_last;
    }
    
    for ( auto& th : threads )
        th.join();

    // accumulate results
    equal = std::accumulate( local_equals.begin(), local_equals.end(), 0 );
    diff  = std::accumulate( local_diffs.begin(), local_diffs.end(), 0 );
}

static IndexOpsMap build_index_ops()
{
    // produce both handler and comparator for a given index type
    auto make_ops = []<typename Index>() -> IndexOps {
        IndexOps ops;
        ops.sizeHandler = [] ( steem::chain::database& db, int& size ) {
            try 
            {
                // we only use the default index `by_id`
                const auto& idx = db.get_index<Index>().indices().template get<steem::chain::by_id>();
                size = static_cast<int>( idx.size() );
            } 
            catch( ... ) 
            {
                size = -1; 
            }
        };
        ops.comparator = [] ( steem::chain::database& db1, steem::chain::database& db2, const fc::string& index_name, int& equal, int& diff ) {
            try 
            {
                const auto& idx1 = db1.get_index<Index>().indices().template get<steem::chain::by_id>();
                const auto& idx2 = db2.get_index<Index>().indices().template get<steem::chain::by_id>();

                // time measurement start
                // const auto start{std::chrono::steady_clock::now()};
                compare_indices( idx1, idx2, index_name, equal, diff );
                // time measurement end
                // const auto finish{std::chrono::steady_clock::now()};
                // const std::chrono::duration<double> elapsed_seconds{finish - start};
                // std::cout << "Time to compare index " << index_name << ": " << elapsed_seconds.count() << "s\n";                
            }
            catch ( const std::exception& e )
            {
                std::cerr << "Comparator error for " << index_name << ": " << e.what() << std::endl;
            }
            catch (...) 
            {
                std::cerr << "Unknown comparator error for " << index_name << std::endl;
            }
        };
        return ops;
    };

    IndexOpsMap m;
    // register ops per index
    m["comment_index"] = make_ops.operator()< steem::chain::comment_index >();
    m["comment_content_index"] = make_ops.operator()< steem::chain::comment_content_index >();
    m["comment_vote_index"] = make_ops.operator()< steem::chain::comment_vote_index >();
    m["account_index"] = make_ops.operator()< steem::chain::account_index >();
    m["account_metadata_index"] = make_ops.operator()< steem::chain::account_metadata_index >();
    m["account_authority_index"] = make_ops.operator()< steem::chain::account_authority_index >();
    m["vesting_delegation_index"] = make_ops.operator()< steem::chain::vesting_delegation_index >();
    m["vesting_delegation_expiration_index"] = make_ops.operator()< steem::chain::vesting_delegation_expiration_index >();
    m["owner_authority_history_index"] = make_ops.operator()< steem::chain::owner_authority_history_index >();
    m["account_recovery_request_index"] = make_ops.operator()< steem::chain::account_recovery_request_index >();
    m["change_recovery_account_request_index"] = make_ops.operator()< steem::chain::change_recovery_account_request_index >();
    m["limit_order_index"] = make_ops.operator()< steem::chain::limit_order_index >();
    m["feed_history_index"] = make_ops.operator()< steem::chain::feed_history_index >();
    m["convert_request_index"] = make_ops.operator()< steem::chain::convert_request_index >();
    m["liquidity_reward_balance_index"] = make_ops.operator()< steem::chain::liquidity_reward_balance_index >();
    m["withdraw_vesting_route_index"] = make_ops.operator()< steem::chain::withdraw_vesting_route_index >();
    m["savings_withdraw_index"] = make_ops.operator()< steem::chain::savings_withdraw_index >();
    m["escrow_index"] = make_ops.operator()< steem::chain::escrow_index >();
    m["decline_voting_rights_request_index"] = make_ops.operator()< steem::chain::decline_voting_rights_request_index >();
    m["reward_fund_index"] = make_ops.operator()< steem::chain::reward_fund_index >();
    m["block_summary_index"] = make_ops.operator()< steem::chain::block_summary_index >();
    m["witness_index"] = make_ops.operator()< steem::chain::witness_index >();
    m["witness_vote_index"] = make_ops.operator()< steem::chain::witness_vote_index >();
    m["witness_schedule_index"] = make_ops.operator()< steem::chain::witness_schedule_index >();
    m["transaction_index"] = make_ops.operator()< steem::chain::transaction_index >();
    m["dynamic_global_property_index"] = make_ops.operator()< steem::chain::dynamic_global_property_index >();
    m["operation_index"] = make_ops.operator()< steem::chain::operation_index >();
    m["account_history_index"] = make_ops.operator()< steem::chain::account_history_index >();
    m["proposal_index"] = make_ops.operator()< steem::chain::proposal_index >();
    m["proposal_vote_index"] = make_ops.operator()< steem::chain::proposal_vote_index >();
    m["transaction_status_index"] = make_ops.operator()< steem::plugins::transaction_status::transaction_status_index >();
    m["bucket_index"] = make_ops.operator()< steem::plugins::market_history::bucket_index >();
    m["order_history_index"] = make_ops.operator()< steem::plugins::market_history::order_history_index >();
    m["reputation_index"] = make_ops.operator()< steem::plugins::reputation::reputation_index >();
    m["key_lookup_index"] = make_ops.operator()< steem::plugins::account_by_key::key_lookup_index >();
    m["witness_custom_op_index"] = make_ops.operator()< steem::plugins::witness::witness_custom_op_index >();
    m["volatile_operation_index"] = make_ops.operator()< steem::plugins::account_history_rocksdb::volatile_operation_index >();
    m["rc_account_index"] = make_ops.operator()< steem::plugins::rc::rc_account_index >();
    m["rc_resource_param_index"] = make_ops.operator()< steem::plugins::rc::rc_resource_param_index >();
    m["rc_pool_index"] = make_ops.operator()< steem::plugins::rc::rc_pool_index >();
    m["hardfork_property_index"] = make_ops.operator()< steem::chain::hardfork_property_index >();
    // nothing happens in these indices
    // m["pending_optional_action_index"] = make_ops.operator()< steem::chain::pending_optional_action_index >();
    // m["pending_required_action_index"] = make_ops.operator()< steem::chain::pending_required_action_index >();

    return m;
}

[[maybe_unused]] static void print_index_delegates( steem::chain::database& db )
{
    std::cout << "-------------------------------------------------------" << std::endl;
    std::cout << "Available index delegates:" << std::endl;
    for( const auto& kv : db.index_delegates() )
    {
        std::cout << " - " << kv.first << std::endl;
    }
}

[[maybe_unused]] static void print_object_types( steem::chain::database& db )
{
    std::cout << "-------------------------------------------------------" << std::endl;
    std::cout << "Registered object types:" << std::endl;
    const auto& abstract_index_cntr = db.get_abstract_index_cntr();
    {
        for ( auto& idx : abstract_index_cntr )
        {
            // idx = chainbase::abstract_index*
            // sind die gleichen infos wie in replay_benchmark ausgegeben wird

            auto info = idx->get_statistics(true);
            std::cout << " - " << info._value_type_name << ": item_count: " << info._item_count << "| item_sizeof: " << info._item_sizeof << std::endl;
        }
    }
}

static void open_database( steem::chain::database& db, const fc::path& base_path, steem::chain::database::open_args& db_open_args)
{
    fc::variant database_config;

    const auto db_config_path = base_path / "database.cfg";
    database_config = fc::json::from_file( db_config_path, fc::json::strict_parser );

    db_open_args.data_dir = base_path / "/blockchain";
    db_open_args.shared_mem_dir = base_path / "/blockchain";
    db_open_args.database_cfg = database_config;

    // open database
    std::cout << "-------------------------------------------------------" << std::endl;
    std::cout << "Open database from " << base_path.string() << std::endl;
    db.open( db_open_args );
}

int main(int argc, char** argv)
{
    // database
    steem::chain::database db1;
    steem::chain::database db2;
    steem::chain::database::open_args db1_open_args;
    steem::chain::database::open_args db2_open_args;

    if (argc < 3)
    {
       std::cerr << "Usage: rocksdb_dump <chain_directory_db1> <chain_directory_db2>" << std::endl;
       return 2;
    }
    
    open_database( db1, fc::path( argv[1] ), db1_open_args );
    const std::vector<fc::string> db1_data_indices = get_indices_from_subdirs( db1_open_args.data_dir );
    print_index_delegates( db1 );
    // print_object_types( db1 );
    
    open_database( db2, fc::path( argv[2] ), db2_open_args );
    const std::vector<fc::string> db2_data_indices = get_indices_from_subdirs( db2_open_args.data_dir );
    print_index_delegates( db2 );
    
    // build combined ops (handlers + comparators) per index type
    const IndexOpsMap idx_ops = build_index_ops();

    // print comparison of data indices
    print_index_numbers( db1_data_indices, db2_data_indices );

    std::cout << "-------------------------------------------------------" << std::endl;
    std::cout << "Index comparison (DB1 | DB2):" << std::endl;

    // base database is db1
    for ( const auto& idx_name : db1_data_indices )
    {
        auto it = idx_ops.find( idx_name );
        int size1 = -1;
        int size2 = -1;
        bool both_indices_exist = false;
        int equal = -1;
        int diff = -1;

        if( it != idx_ops.end() && it->second.sizeHandler )
        {
            // run size handler for db1 and for db2 if index exists there
            it->second.sizeHandler( db1, size1 );
            if (std::find(db2_data_indices.begin(), db2_data_indices.end(), idx_name) != db2_data_indices.end())
            {
                it->second.sizeHandler( db2, size2 );
                both_indices_exist = true;
            }
            print_index_sizes( idx_name, size1, size2 );

            // run comparator if available and index exists in both DBs
            if ( both_indices_exist && it->second.comparator )
            {
                it->second.comparator( db1, db2, idx_name, equal, diff );
                print_index_compare_results( equal, diff);
            }
        }
        else
        {
            std::cout << "No runtime handler registered for index: " << idx_name << std::endl;
        }
    }

    db1.close();
    db2.close();

    return 0;
}
