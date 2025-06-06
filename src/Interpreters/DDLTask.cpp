#include <Interpreters/DDLTask.h>
#include <base/sort.h>
#include <Common/DNSResolver.h>
#include <Common/OpenTelemetryTraceContext.h>
#include <Common/isLocalAddress.h>
#include <Core/Settings.h>
#include <Databases/DatabaseReplicated.h>
#include <Interpreters/DatabaseCatalog.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/Operators.h>
#include <IO/ReadBufferFromString.h>
#include <Poco/Net/NetException.h>
#include <Common/logger_useful.h>
#include <Parsers/ASTQueryWithOnCluster.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <Parsers/ASTQueryWithTableAndOutput.h>


namespace DB
{

namespace Setting
{
    extern const SettingsBool allow_settings_after_format_in_insert;
    extern const SettingsUInt64 distributed_ddl_entry_format_version;
    extern const SettingsUInt64 log_queries_cut_to_length;
    extern const SettingsUInt64 max_parser_depth;
    extern const SettingsUInt64 max_parser_backtracks;
    extern const SettingsUInt64 max_query_size;
}

namespace ErrorCodes
{
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int UNKNOWN_TYPE_OF_QUERY;
    extern const int INCONSISTENT_CLUSTER_DEFINITION;
    extern const int LOGICAL_ERROR;
    extern const int DNS_ERROR;
}


HostID HostID::fromString(const String & host_port_str)
{
    HostID res;
    std::tie(res.host_name, res.port) = Cluster::Address::fromString(host_port_str);
    return res;
}


bool HostID::isLocalAddress(UInt16 clickhouse_port) const
{
    try
    {
        return DB::isLocalAddress(DNSResolver::instance().resolveAddress(host_name, port), clickhouse_port);
    }
    catch (const DB::NetException &)
    {
        /// Avoid "Host not found" exceptions
        return false;
    }
    catch (const Poco::Net::NetException &)
    {
        /// Avoid "Host not found" exceptions
        return false;
    }
}

void DDLLogEntry::assertVersion() const
{
    if (version == 0
    /// NORMALIZE_CREATE_ON_INITIATOR_VERSION does not change the entry format, it uses versioin 2, so there shouldn't be such version
    || version == NORMALIZE_CREATE_ON_INITIATOR_VERSION
    || version > DDL_ENTRY_FORMAT_MAX_VERSION)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION, "Unknown DDLLogEntry format version: {}."
                                                            "Maximum supported version is {}", version, DDL_ENTRY_FORMAT_MAX_VERSION);
}

void DDLLogEntry::setSettingsIfRequired(ContextPtr context)
{
    version = context->getSettingsRef()[Setting::distributed_ddl_entry_format_version];
    if (version <= 0 || version > DDL_ENTRY_FORMAT_MAX_VERSION)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION, "Unknown distributed_ddl_entry_format_version: {}."
                                                            "Maximum supported version is {}.", version, DDL_ENTRY_FORMAT_MAX_VERSION);

    parent_table_uuid = context->getParentTable();
    if (parent_table_uuid.has_value())
        version = std::max(version, PARENT_TABLE_UUID_VERSION);

    /// NORMALIZE_CREATE_ON_INITIATOR_VERSION does not affect entry format in ZooKeeper
    if (version == NORMALIZE_CREATE_ON_INITIATOR_VERSION)
        version = SETTINGS_IN_ZK_VERSION;

    if (version >= SETTINGS_IN_ZK_VERSION)
        settings.emplace(context->getSettingsRef().changes());
}

String DDLLogEntry::toString() const
{
    WriteBufferFromOwnString wb;

    wb << "version: " << version << "\n";
    wb << "query: " << escape << query << "\n";

    bool write_hosts = version == OLDEST_VERSION || !hosts.empty();
    if (write_hosts)
    {
        Strings host_id_strings(hosts.size());
        std::transform(hosts.begin(), hosts.end(), host_id_strings.begin(), HostID::applyToString);
        wb << "hosts: " << host_id_strings << "\n";
    }

    wb << "initiator: " << initiator << "\n";

    bool write_settings = SETTINGS_IN_ZK_VERSION <= version && settings && !settings->empty();
    if (write_settings)
    {
        ASTSetQuery ast;
        ast.is_standalone = false;
        ast.changes = *settings;
        wb << "settings: " << ast.formatWithSecretsOneLine() << "\n";
    }

    if (version >= OPENTELEMETRY_ENABLED_VERSION)
        wb << "tracing: " << this->tracing_context;
    /// NOTE: OPENTELEMETRY_ENABLED_VERSION has new line in TracingContext::serialize(), so no need to add one more

    if (version >= PRESERVE_INITIAL_QUERY_ID_VERSION)
    {
        writeString("initial_query_id: ", wb);
        writeEscapedString(initial_query_id, wb);
        writeChar('\n', wb);
    }

    if (version >= BACKUP_RESTORE_FLAG_IN_ZK_VERSION)
        wb << "is_backup_restore: " << is_backup_restore << "\n";

    if (version >= PARENT_TABLE_UUID_VERSION)
    {
        wb << "parent: ";
        if (parent_table_uuid.has_value())
            wb << parent_table_uuid.value();
        else
            wb << "-";
        wb << "\n";
    }

    return wb.str();
}

void DDLLogEntry::parse(const String & data)
{
    ReadBufferFromString rb(data);

    rb >> "version: " >> version >> "\n";
    assertVersion();

    Strings host_id_strings;
    rb >> "query: " >> escape >> query >> "\n";
    if (version == OLDEST_VERSION)
    {
        rb >> "hosts: " >> host_id_strings >> "\n";

        if (!rb.eof())
            rb >> "initiator: " >> initiator >> "\n";
        else
            initiator.clear();
    }
    else if (version >= SETTINGS_IN_ZK_VERSION)
    {
        if (!rb.eof() && *rb.position() == 'h')
            rb >> "hosts: " >> host_id_strings >> "\n";
        if (!rb.eof() && *rb.position() == 'i')
            rb >> "initiator: " >> initiator >> "\n";
        if (!rb.eof() && *rb.position() == 's')
        {
            String settings_str;
            rb >> "settings: " >> settings_str >> "\n";
            ParserSetQuery parser{true};
            constexpr UInt64 max_depth = 16;
            constexpr UInt64 max_backtracks = DBMS_DEFAULT_MAX_PARSER_BACKTRACKS;
            ASTPtr settings_ast = parseQuery(
                parser, settings_str, Context::getGlobalContextInstance()->getSettingsRef()[Setting::max_query_size], max_depth, max_backtracks);
            settings.emplace(std::move(settings_ast->as<ASTSetQuery>()->changes));
        }
    }

    if (version >= OPENTELEMETRY_ENABLED_VERSION)
    {
        if (!rb.eof() && *rb.position() == 't')
            rb >> "tracing: " >> this->tracing_context;
    }

    if (version >= PRESERVE_INITIAL_QUERY_ID_VERSION)
    {
        checkString("initial_query_id: ", rb);
        readEscapedString(initial_query_id, rb);
        checkChar('\n', rb);
    }

    if (version >= BACKUP_RESTORE_FLAG_IN_ZK_VERSION)
    {
        checkString("is_backup_restore: ", rb);
        readBoolText(is_backup_restore, rb);
        checkChar('\n', rb);
    }

    if (version >= PARENT_TABLE_UUID_VERSION)
    {
        rb >> "parent: ";
        if (!checkChar('-', rb))
        {
            UUID uuid;
            rb >> uuid;
            parent_table_uuid = uuid;
        }
        rb >> "\n";
    }

    assertEOF(rb);

    if (!host_id_strings.empty())
    {
        hosts.resize(host_id_strings.size());
        std::transform(host_id_strings.begin(), host_id_strings.end(), hosts.begin(), HostID::fromString);
    }
}


void DDLTaskBase::parseQueryFromEntry(ContextPtr context)
{
    const char * begin = entry.query.data();
    const char * end = begin + entry.query.size();
    const auto & settings = context->getSettingsRef();

    ParserQuery parser_query(end, settings[Setting::allow_settings_after_format_in_insert]);
    String description;
    query = parseQuery(parser_query, begin, end, description, 0, settings[Setting::max_parser_depth], settings[Setting::max_parser_backtracks]);
}

void DDLTaskBase::formatRewrittenQuery(ContextPtr context)
{
    /// Convert rewritten AST back to string.
    query_str = query->formatWithSecretsOneLine();
    query_for_logging = query->formatForLogging(context->getSettingsRef()[Setting::log_queries_cut_to_length]);
}

ContextMutablePtr DDLTaskBase::makeQueryContext(ContextPtr from_context, const ZooKeeperPtr & /*zookeeper*/)
{
    auto query_context = Context::createCopy(from_context);
    query_context->makeQueryContext();
    query_context->setCurrentQueryId(""); // generate random query_id
    query_context->setQueryKind(ClientInfo::QueryKind::SECONDARY_QUERY);
    if (entry.settings)
        query_context->applySettingsChanges(*entry.settings);
    return query_context;
}


bool DDLTask::findCurrentHostID(ContextPtr global_context, LoggerPtr log, const ZooKeeperPtr & zookeeper, const std::optional<std::string> & config_host_name)
{
    bool host_in_hostlist = false;
    std::exception_ptr first_exception = nullptr;

    const auto maybe_secure_port = global_context->getTCPPortSecure();
    const auto port = global_context->getTCPPort();

    if (config_host_name)
    {
        bool is_local_port = (maybe_secure_port && HostID(*config_host_name, *maybe_secure_port).isLocalAddress(*maybe_secure_port)) ||
                             HostID(*config_host_name, port).isLocalAddress(port);

        if (!is_local_port)
            throw Exception(
                ErrorCodes::DNS_ERROR,
                "{} is not a local address. Check parameter 'host_name' in the configuration",
                *config_host_name);
    }

    for (const HostID & host : entry.hosts)
    {
        if (config_host_name)
        {
            if (config_host_name != host.host_name)
                continue;

            if (maybe_secure_port != host.port && port != host.port)
                continue;

            host_in_hostlist = true;
            host_id = host;
            host_id_str = host.toString();
            break;
        }

        try
        {
            /// The port is considered local if it matches TCP or TCP secure port that the server is listening.
            bool is_local_port
                = (maybe_secure_port && host.isLocalAddress(*maybe_secure_port)) || host.isLocalAddress(port);

            if (!is_local_port)
                continue;
        }
        catch (const Exception & e)
        {
            if (e.code() != ErrorCodes::DNS_ERROR)
                throw;

            if (!first_exception)
                first_exception = std::current_exception();

            /// Ignore unknown hosts (in case DNS record was removed)
            /// We will rethrow exception if we don't find local host in the list.
            continue;
        }

        if (host_in_hostlist)
        {
            /// This check could be slow a little bit
            LOG_WARNING(log, "There are two the same ClickHouse instances in task {}: {} and {}. Will use the first one only.",
                             entry_name, host_id.readableString(), host.readableString());
        }
        else
        {
            host_in_hostlist = true;
            host_id = host;
            host_id_str = host.toString();
        }
    }

    if (!host_in_hostlist && first_exception)
    {
        if (zookeeper->exists(getFinishedNodePath()))
        {
            LOG_WARNING(log, "Failed to find current host ID, but assuming that {} is finished because {} exists. Skipping the task. Error: {}",
                        entry_name, getFinishedNodePath(), getExceptionMessage(first_exception, /*with_stacktrace*/ true));
            return false;
        }

        size_t finished_nodes_count = zookeeper->getChildren(fs::path(entry_path) / "finished").size();
        if (entry.hosts.size() == finished_nodes_count)
        {
            LOG_WARNING(log, "Failed to find current host ID, but assuming that {} is finished because the number of finished nodes ({}) "
                        "equals to the number of hosts in list. Skipping the task. Error: {}",
                        entry_name, finished_nodes_count, getExceptionMessage(first_exception, /*with_stacktrace*/ true));
            return false;
        }

        /// We don't know for sure if we should process task or not
        std::rethrow_exception(first_exception);
    }

    return host_in_hostlist;
}

void DDLTask::setClusterInfo(ContextPtr context, LoggerPtr log)
{
    auto * query_on_cluster = dynamic_cast<ASTQueryWithOnCluster *>(query.get());
    if (!query_on_cluster)
        throw Exception(ErrorCodes::UNKNOWN_TYPE_OF_QUERY, "Received unknown DDL query");

    cluster_name = query_on_cluster->cluster;
    cluster = context->tryGetCluster(cluster_name);

    if (!cluster)
        throw Exception(ErrorCodes::INCONSISTENT_CLUSTER_DEFINITION,
                        "DDL task {} contains current host {} in cluster {}, but there is no such cluster here.",
                        entry_name, host_id.readableString(), cluster_name);

    /// Try to find host from task host list in cluster
    /// At the first, try find exact match (host name and ports should be literally equal)
    /// If the attempt fails, try find it resolving host name of each instance

    if (!tryFindHostInCluster())
    {
        LOG_WARNING(log, "Not found the exact match of host {} from task {} in cluster {} definition. Will try to find it using host name resolving.",
                         host_id.readableString(), entry_name, cluster_name);

        if (!tryFindHostInClusterViaResolving(context))
            throw Exception(ErrorCodes::INCONSISTENT_CLUSTER_DEFINITION, "Not found host {} in definition of cluster {}",
                                                                 host_id.readableString(), cluster_name);

        LOG_INFO(log, "Resolved host {} from task {} as host {} in definition of cluster {}",
                 host_id.readableString(), entry_name, address_in_cluster.readableString(), cluster_name);
    }

    /// Rewrite AST without ON CLUSTER.
    WithoutOnClusterASTRewriteParams params;
    params.default_database = address_in_cluster.default_database;
    params.host_id = address_in_cluster.toString();
    query = query_on_cluster->getRewrittenASTWithoutOnCluster(params);
    query_on_cluster = nullptr;
}

bool DDLTask::tryFindHostInCluster()
{
    const auto & shards = cluster->getShardsAddresses();
    bool found_exact_match = false;
    String default_database;

    for (size_t shard_num = 0; shard_num < shards.size(); ++shard_num)
    {
        for (size_t replica_num = 0; replica_num < shards[shard_num].size(); ++replica_num)
        {
            const Cluster::Address & address = shards[shard_num][replica_num];

            if (address.host_name == host_id.host_name && address.port == host_id.port)
            {
                if (found_exact_match)
                {
                    if (default_database == address.default_database)
                    {
                        throw Exception(ErrorCodes::INCONSISTENT_CLUSTER_DEFINITION,
                                        "There are two exactly the same ClickHouse instances {} in cluster {}",
                                        address.readableString(), cluster_name);
                    }

                    /* Circular replication is used.
                         * It is when every physical node contains
                         * replicas of different shards of the same table.
                         * To distinguish one replica from another on the same node,
                         * every shard is placed into separate database.
                         * */
                    is_circular_replicated = true;
                    auto * query_with_table = dynamic_cast<ASTQueryWithTableAndOutput *>(query.get());

                    /// For other DDLs like CREATE USER, there is no database name and should be executed successfully.
                    if (query_with_table)
                    {
                        if (!query_with_table->database)
                            throw Exception(
                                ErrorCodes::INCONSISTENT_CLUSTER_DEFINITION,
                                "For a distributed DDL on circular replicated cluster its table name "
                                "must be qualified by database name.");

                        if (default_database == query_with_table->getDatabase())
                            return true;
                    }
                }
                found_exact_match = true;
                host_shard_num = shard_num;
                host_replica_num = replica_num;
                address_in_cluster = address;
                default_database = address.default_database;
            }
        }
    }

    return found_exact_match;
}

bool DDLTask::tryFindHostInClusterViaResolving(ContextPtr context)
{
    const auto & shards = cluster->getShardsAddresses();
    bool found_via_resolving = false;

    for (size_t shard_num = 0; shard_num < shards.size(); ++shard_num)
    {
        for (size_t replica_num = 0; replica_num < shards[shard_num].size(); ++replica_num)
        {
            const Cluster::Address & address = shards[shard_num][replica_num];

            if (auto resolved = address.getResolvedAddress(); resolved
                && (isLocalAddress(*resolved, context->getTCPPort())
                    || (context->getTCPPortSecure() && isLocalAddress(*resolved, *context->getTCPPortSecure()))))
            {
                if (found_via_resolving)
                {
                    throw Exception(ErrorCodes::INCONSISTENT_CLUSTER_DEFINITION,
                                    "There are two the same ClickHouse instances in cluster {} : {} and {}",
                                    cluster_name, address_in_cluster.readableString(), address.readableString());
                }

                found_via_resolving = true;
                host_shard_num = shard_num;
                host_replica_num = replica_num;
                address_in_cluster = address;
            }
        }
    }

    return found_via_resolving;
}

String DDLTask::getShardID() const
{
    /// Generate unique name for shard node, it will be used to execute the query by only single host
    /// Shard node name has format 'replica_name1,replica_name2,...,replica_nameN'
    /// Where replica_name is 'replica_config_host_name:replica_port'

    auto shard_addresses = cluster->getShardsAddresses().at(host_shard_num);

    Strings replica_names;
    for (const Cluster::Address & address : shard_addresses)
        replica_names.emplace_back(address.readableString());
    ::sort(replica_names.begin(), replica_names.end());

    String res;
    for (auto it = replica_names.begin(); it != replica_names.end(); ++it)
        res += *it + (std::next(it) != replica_names.end() ? "," : "");

    return res;
}

DatabaseReplicatedTask::DatabaseReplicatedTask(const String & name, const String & path, DatabaseReplicated * database_)
    : DDLTaskBase(name, path)
    , database(database_)
{
    host_id_str = database->getFullReplicaName();
}

String DatabaseReplicatedTask::getShardID() const
{
    return database->shard_name;
}

void DatabaseReplicatedTask::parseQueryFromEntry(ContextPtr context)
{
    DDLTaskBase::parseQueryFromEntry(context);
    if (auto * ddl_query = dynamic_cast<ASTQueryWithTableAndOutput *>(query.get()))
    {
        /// Update database name with actual name of local database
        chassert(!ddl_query->database);
        ddl_query->setDatabase(database->getDatabaseName());
    }
    formatRewrittenQuery(context);
}

ContextMutablePtr DatabaseReplicatedTask::makeQueryContext(ContextPtr from_context, const ZooKeeperPtr & zookeeper)
{
    auto query_context = DDLTaskBase::makeQueryContext(from_context, zookeeper);
    query_context->setQueryKind(ClientInfo::QueryKind::SECONDARY_QUERY);
    query_context->setQueryKindReplicatedDatabaseInternal();
    query_context->setCurrentDatabase(database->getDatabaseName());

    auto txn = std::make_shared<ZooKeeperMetadataTransaction>(zookeeper, database->zookeeper_path, is_initial_query, entry_path);
    query_context->initZooKeeperMetadataTransaction(txn);

    if (is_initial_query)
    {
        txn->addOp(zkutil::makeRemoveRequest(entry_path + "/try", -1));
        txn->addOp(zkutil::makeCreateRequest(entry_path + "/committed", host_id_str, zkutil::CreateMode::Persistent));
        txn->addOp(zkutil::makeSetRequest(database->zookeeper_path + "/max_log_ptr", toString(getLogEntryNumber(entry_name)), -1));
    }

    txn->addOp(getOpToUpdateLogPointer());

    for (auto & op : ops)
        txn->addOp(std::move(op));
    ops.clear();

    return query_context;
}

Coordination::RequestPtr DatabaseReplicatedTask::getOpToUpdateLogPointer()
{
    return zkutil::makeSetRequest(database->replica_path + "/log_ptr", toString(getLogEntryNumber(entry_name)), -1);
}

void DatabaseReplicatedTask::createSyncedNodeIfNeed(const ZooKeeperPtr & zookeeper)
{
    assert(!completely_processed);
    if (!entry.settings)
        return;

    Field value;
    if (!entry.settings->tryGet("database_replicated_enforce_synchronous_settings", value))
        return;

    /// Bool type is really weird, sometimes it's Bool and sometimes it's UInt64...
    assert(value.getType() == Field::Types::Bool || value.getType() == Field::Types::UInt64);
    if (!value.safeGet<UInt64>())
        return;

    zookeeper->createIfNotExists(getSyncedNodePath(), "");
}

String DDLTaskBase::getLogEntryName(UInt32 log_entry_number)
{
    return zkutil::getSequentialNodeName("query-", log_entry_number);
}

UInt32 DDLTaskBase::getLogEntryNumber(const String & log_entry_name)
{
    constexpr const char * name = "query-";
    assert(startsWith(log_entry_name, name));
    UInt32 num = parse<UInt32>(log_entry_name.substr(strlen(name)));
    assert(num < std::numeric_limits<Int32>::max());
    return num;
}

void ZooKeeperMetadataTransaction::commit()
{
    if (state != CREATED)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Incorrect state ({}), it's a bug", state);
    state = FAILED;
    current_zookeeper->multi(ops, /* check_session_valid */ true);
    state = COMMITTED;

    if (finalizer)
    {
        finalizer();
        finalizer = FinalizerCallback();
    }
}

ClusterPtr tryGetReplicatedDatabaseCluster(const String & cluster_name)
{
    String name = cluster_name;
    bool all_groups = false;
    if (name.starts_with(DatabaseReplicated::ALL_GROUPS_CLUSTER_PREFIX))
    {
        name = name.substr(strlen(DatabaseReplicated::ALL_GROUPS_CLUSTER_PREFIX));
        all_groups = true;
    }

    if (const auto * replicated_db = dynamic_cast<const DatabaseReplicated *>(DatabaseCatalog::instance().tryGetDatabase(name).get()))
    {
        if (all_groups)
            return replicated_db->tryGetAllGroupsCluster();
        return replicated_db->tryGetCluster();
    }
    return {};
}

}
