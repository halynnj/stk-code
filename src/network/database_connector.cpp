//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2013-2015 SuperTuxKart-Team
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#ifdef ENABLE_SQLITE3

#include "network/database_connector.hpp"

#include "network/network_player_profile.hpp"
#include "network/server_config.hpp"
#include "network/socket_address.hpp"
#include "network/stk_host.hpp"
#include "network/stk_ipv6.hpp"
#include "network/stk_peer.hpp"
#include "utils/log.hpp"

//-----------------------------------------------------------------------------
void DatabaseConnector::initDatabase()
{
    m_last_poll_db_time = StkTime::getMonoTimeMs();
    m_db = NULL;
    m_ip_ban_table_exists = false;
    m_ipv6_ban_table_exists = false;
    m_online_id_ban_table_exists = false;
    m_ip_geolocation_table_exists = false;
    m_ipv6_geolocation_table_exists = false;
    m_player_reports_table_exists = false;
    if (!ServerConfig::m_sql_management)
        return;
    const std::string& path = ServerConfig::getConfigDirectory() + "/" +
        ServerConfig::m_database_file.c_str();
    int ret = sqlite3_open_v2(path.c_str(), &m_db,
        SQLITE_OPEN_SHAREDCACHE | SQLITE_OPEN_FULLMUTEX |
        SQLITE_OPEN_READWRITE, NULL);
    if (ret != SQLITE_OK)
    {
        Log::error("ServerLobby", "Cannot open database: %s.",
            sqlite3_errmsg(m_db));
        sqlite3_close(m_db);
        m_db = NULL;
        return;
    }
    sqlite3_busy_handler(m_db, [](void* data, int retry)
        {
            int retry_count = ServerConfig::m_database_timeout / 100;
            if (retry < retry_count)
            {
                sqlite3_sleep(100);
                // Return non-zero to let caller retry again
                return 1;
            }
            // Return zero to let caller return SQLITE_BUSY immediately
            return 0;
        }, NULL);
    sqlite3_create_function(m_db, "insideIPv6CIDR", 2, SQLITE_UTF8, NULL,
        &insideIPv6CIDRSQL, NULL, NULL);
    sqlite3_create_function(m_db, "upperIPv6", 1, SQLITE_UTF8, NULL,
        &upperIPv6SQL, NULL, NULL);
    checkTableExists(ServerConfig::m_ip_ban_table, m_ip_ban_table_exists);
    checkTableExists(ServerConfig::m_ipv6_ban_table, m_ipv6_ban_table_exists);
    checkTableExists(ServerConfig::m_online_id_ban_table,
        m_online_id_ban_table_exists);
    checkTableExists(ServerConfig::m_player_reports_table,
        m_player_reports_table_exists);
    checkTableExists(ServerConfig::m_ip_geolocation_table,
        m_ip_geolocation_table_exists);
    checkTableExists(ServerConfig::m_ipv6_geolocation_table,
        m_ipv6_geolocation_table_exists);
}   // initDatabase

//-----------------------------------------------------------------------------
void DatabaseConnector::destroyDatabase()
{
    auto peers = STKHost::get()->getPeers();
    for (auto& peer : peers)
        writeDisconnectInfoTable(peer.get());
    if (m_db != NULL)
        sqlite3_close(m_db);
}   // destroyDatabase

//-----------------------------------------------------------------------------
/** Run simple query with write lock waiting and optional function, this
 *  function has no callback for the return (if any) by the query.
 *  Return true if no error occurs
 */
bool DatabaseConnector::easySQLQuery(const std::string& query,
                   std::function<void(sqlite3_stmt* stmt)> bind_function) const
{
    if (!m_db)
        return false;
    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        if (bind_function)
            bind_function(stmt);
        ret = sqlite3_step(stmt);
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("DatabaseConnector",
                "Error finalize database for easy query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
            return false;
        }
    }
    else
    {
        Log::error("DatabaseConnector",
            "Error preparing database for easy query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}   // easySQLQuery

//-----------------------------------------------------------------------------
/* Write true to result if table name exists in database. */
void DatabaseConnector::checkTableExists(const std::string& table, bool& result)
{
    if (!m_db)
        return;
    sqlite3_stmt* stmt = NULL;
    if (!table.empty())
    {
        std::string query = StringUtils::insertValues(
            "SELECT count(type) FROM sqlite_master "
            "WHERE type='table' AND name='%s';", table.c_str());

        int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
        if (ret == SQLITE_OK)
        {
            ret = sqlite3_step(stmt);
            if (ret == SQLITE_ROW)
            {
                int number = sqlite3_column_int(stmt, 0);
                if (number == 1)
                {
                    Log::info("DatabaseConnector", "Table named %s will be used.",
                        table.c_str());
                    result = true;
                }
            }
            ret = sqlite3_finalize(stmt);
            if (ret != SQLITE_OK)
            {
                Log::error("DatabaseConnector",
                    "Error finalize database for query %s: %s",
                    query.c_str(), sqlite3_errmsg(m_db));
            }
        }
    }
    if (!result && !table.empty())
    {
        Log::warn("DatabaseConnector", "Table named %s not found in database.",
            table.c_str());
    }
}   // checkTableExists

//-----------------------------------------------------------------------------
std::string DatabaseConnector::ip2Country(const SocketAddress& addr) const
{
    if (!m_db || !m_ip_geolocation_table_exists || addr.isLAN())
        return "";

    std::string cc_code;
    std::string query = StringUtils::insertValues(
        "SELECT country_code FROM %s "
        "WHERE `ip_start` <= %d AND `ip_end` >= %d "
        "ORDER BY `ip_start` DESC LIMIT 1;",
        ServerConfig::m_ip_geolocation_table.c_str(), addr.getIP(),
        addr.getIP());

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            const char* country_code = (char*)sqlite3_column_text(stmt, 0);
            cc_code = country_code;
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("DatabaseConnector",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("DatabaseConnector", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return "";
    }
    return cc_code;
}   // ip2Country

//-----------------------------------------------------------------------------
std::string DatabaseConnector::ipv62Country(const SocketAddress& addr) const
{
    if (!m_db || !m_ipv6_geolocation_table_exists)
        return "";

    std::string cc_code;
    const std::string& ipv6 = addr.toString(false/*show_port*/);
    std::string query = StringUtils::insertValues(
        "SELECT country_code FROM %s "
        "WHERE `ip_start` <= upperIPv6(\"%s\") AND `ip_end` >= upperIPv6(\"%s\") "
        "ORDER BY `ip_start` DESC LIMIT 1;",
        ServerConfig::m_ipv6_geolocation_table.c_str(), ipv6.c_str(),
        ipv6.c_str());

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW)
        {
            const char* country_code = (char*)sqlite3_column_text(stmt, 0);
            cc_code = country_code;
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("DatabaseConnector",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("DatabaseConnector", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return "";
    }
    return cc_code;
}   // ipv62Country

// ----------------------------------------------------------------------------
void DatabaseConnector::upperIPv6SQL(sqlite3_context* context, int argc,
                         sqlite3_value** argv)
{
    if (argc != 1)
    {
        sqlite3_result_int64(context, 0);
        return;
    }

    char* ipv6 = (char*)sqlite3_value_text(argv[0]);
    if (ipv6 == NULL)
    {
        sqlite3_result_int64(context, 0);
        return;
    }
    sqlite3_result_int64(context, upperIPv6(ipv6));
}

// ----------------------------------------------------------------------------
void DatabaseConnector::insideIPv6CIDRSQL(sqlite3_context* context, int argc,
                       sqlite3_value** argv)
{
    if (argc != 2)
    {
        sqlite3_result_int(context, 0);
        return;
    }

    char* ipv6_cidr = (char*)sqlite3_value_text(argv[0]);
    char* ipv6_in = (char*)sqlite3_value_text(argv[1]);
    if (ipv6_cidr == NULL || ipv6_in == NULL)
    {
        sqlite3_result_int(context, 0);
        return;
    }
    sqlite3_result_int(context, insideIPv6CIDR(ipv6_cidr, ipv6_in));
}   // insideIPv6CIDRSQL

// ----------------------------------------------------------------------------
/*
Copy below code so it can be use as loadable extension to be used in sqlite3
command interface (together with andIPv6 and insideIPv6CIDR from stk_ipv6)

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
// ----------------------------------------------------------------------------
sqlite3_extension_init(sqlite3* db, char** pzErrMsg,
                       const sqlite3_api_routines* pApi)
{
    SQLITE_EXTENSION_INIT2(pApi)
    sqlite3_create_function(db, "insideIPv6CIDR", 2, SQLITE_UTF8, NULL,
        insideIPv6CIDRSQL, NULL, NULL);
    sqlite3_create_function(db, "upperIPv6", 1, SQLITE_UTF8,  0, upperIPv6SQL,
        0, 0);
    return 0;
}   // sqlite3_extension_init
*/

//-----------------------------------------------------------------------------
void DatabaseConnector::writeDisconnectInfoTable(STKPeer* peer)
{
    if (m_server_stats_table.empty())
        return;
    std::string query = StringUtils::insertValues(
        "UPDATE %s SET disconnected_time = datetime('now'), "
        "ping = %d, packet_loss = %d "
        "WHERE host_id = %u;", m_server_stats_table.c_str(),
        peer->getAveragePing(), peer->getPacketLoss(),
        peer->getHostId());
    easySQLQuery(query);
}   // writeDisconnectInfoTable

//-----------------------------------------------------------------------------

void DatabaseConnector::initServerStatsTable()
{
    if (!ServerConfig::m_sql_management || !m_db)
        return;
    std::string table_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_stats";

    std::ostringstream oss;
    oss << "CREATE TABLE IF NOT EXISTS " << table_name << " (\n"
        "    host_id INTEGER UNSIGNED NOT NULL PRIMARY KEY, -- Unique host id in STKHost of each connection session for a STKPeer\n"
        "    ip INTEGER UNSIGNED NOT NULL, -- IP decimal of host\n";
    if (ServerConfig::m_ipv6_connection)
        oss << "    ipv6 TEXT NOT NULL DEFAULT '', -- IPv6 (if exists) in string of host\n";
    oss << "    port INTEGER UNSIGNED NOT NULL, -- Port of host\n"
        "    online_id INTEGER UNSIGNED NOT NULL, -- Online if of the host (0 for offline account)\n"
        "    username TEXT NOT NULL, -- First player name in the host (if the host has splitscreen player)\n"
        "    player_num INTEGER UNSIGNED NOT NULL, -- Number of player(s) from the host, more than 1 if it has splitscreen player\n"
        "    country_code TEXT NULL DEFAULT NULL, -- 2-letter country code of the host\n"
        "    version TEXT NOT NULL, -- SuperTuxKart version of the host\n"
        "    os TEXT NOT NULL, -- Operating system of the host\n"
        "    connected_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, -- Time when connected\n"
        "    disconnected_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, -- Time when disconnected (saved when disconnected)\n"
        "    ping INTEGER UNSIGNED NOT NULL DEFAULT 0, -- Ping of the host\n"
        "    packet_loss INTEGER NOT NULL DEFAULT 0 -- Mean packet loss count from ENet (saved when disconnected)\n"
        ") WITHOUT ROWID;";
    std::string query = oss.str();
    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        ret = sqlite3_finalize(stmt);
        if (ret == SQLITE_OK)
            m_server_stats_table = table_name;
        else
        {
            Log::error("DatabaseConnector",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("DatabaseConnector", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
    }
    if (m_server_stats_table.empty())
        return;

    // Extra default table _countries:
    // Server owner need to initialise this table himself, check NETWORKING.md
    std::string country_table_name = std::string("v") + StringUtils::toString(
        ServerConfig::m_server_db_version) + "_countries";
    query = StringUtils::insertValues(
        "CREATE TABLE IF NOT EXISTS %s (\n"
        "    country_code TEXT NOT NULL PRIMARY KEY UNIQUE, -- Unique 2-letter country code\n"
        "    country_flag TEXT NOT NULL, -- Unicode country flag representation of 2-letter country code\n"
        "    country_name TEXT NOT NULL -- Readable name of this country\n"
        ") WITHOUT ROWID;", country_table_name.c_str());
    easySQLQuery(query);

    // Default views:
    // _full_stats
    // Full stats with ip in human readable format and time played of each
    // players in minutes
    std::string full_stats_view_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_full_stats";
    oss.str("");
    oss << "CREATE VIEW IF NOT EXISTS " << full_stats_view_name << " AS\n"
        << "    SELECT host_id, ip,\n"
        << "    ((ip >> 24) & 255) ||'.'|| ((ip >> 16) & 255) ||'.'|| ((ip >>  8) & 255) ||'.'|| ((ip ) & 255) AS ip_readable,\n";
    if (ServerConfig::m_ipv6_connection)
        oss << "    ipv6,";
    oss << "    port, online_id, username, player_num,\n"
        << "    " << m_server_stats_table << ".country_code AS country_code, country_flag, country_name, version, os,\n"
        << "    ROUND((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0, 2) AS time_played,\n"
        << "    connected_time, disconnected_time, ping, packet_loss FROM " << m_server_stats_table << "\n"
        << "    LEFT JOIN " << country_table_name << " ON "
        <<      country_table_name << ".country_code = " << m_server_stats_table << ".country_code\n"
        << "    ORDER BY connected_time DESC;";
    query = oss.str();
    easySQLQuery(query);

    // _current_players
    // Current players in server with ip in human readable format and time
    // played of each players in minutes
    std::string current_players_view_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_current_players";
    oss.str("");
    oss.clear();
    oss << "CREATE VIEW IF NOT EXISTS " << current_players_view_name << " AS\n"
        << "    SELECT host_id, ip,\n"
        << "    ((ip >> 24) & 255) ||'.'|| ((ip >> 16) & 255) ||'.'|| ((ip >>  8) & 255) ||'.'|| ((ip ) & 255) AS ip_readable,\n";
    if (ServerConfig::m_ipv6_connection)
        oss << "    ipv6,";
    oss << "    port, online_id, username, player_num,\n"
        << "    " << m_server_stats_table << ".country_code AS country_code, country_flag, country_name, version, os,\n"
        << "    ROUND((STRFTIME(\"%s\", 'now') - STRFTIME(\"%s\", connected_time)) / 60.0, 2) AS time_played,\n"
        << "    connected_time, ping FROM " << m_server_stats_table << "\n"
        << "    LEFT JOIN " << country_table_name << " ON "
        <<      country_table_name << ".country_code = " << m_server_stats_table << ".country_code\n"
        << "    WHERE connected_time = disconnected_time;";
    query = oss.str();
    easySQLQuery(query);

    // _player_stats
    // All players with online id and username with their time played stats
    // in this server since creation of this database
    // If sqlite supports window functions (since 3.25), it will include last session player info (ip, country, ping...)
    std::string player_stats_view_name = std::string("v") +
        StringUtils::toString(ServerConfig::m_server_db_version) + "_" +
        ServerConfig::m_server_uid + "_player_stats";
    oss.str("");
    oss.clear();
    if (sqlite3_libversion_number() < 3025000)
    {
        oss << "CREATE VIEW IF NOT EXISTS " << player_stats_view_name << " AS\n"
            << "    SELECT online_id, username, COUNT(online_id) AS num_connections,\n"
            << "    MIN(connected_time) AS first_connected_time,\n"
            << "    MAX(connected_time) AS last_connected_time,\n"
            << "    ROUND(SUM((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS total_time_played,\n"
            << "    ROUND(AVG((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS average_time_played,\n"
            << "    ROUND(MIN((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS min_time_played,\n"
            << "    ROUND(MAX((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS max_time_played\n"
            << "    FROM " << m_server_stats_table << "\n"
            << "    WHERE online_id != 0 GROUP BY online_id ORDER BY num_connections DESC;";
    }
    else
    {
        oss << "CREATE VIEW IF NOT EXISTS " << player_stats_view_name << " AS\n"
            << "    SELECT a.online_id, a.username, a.ip, a.ip_readable,\n";
        if (ServerConfig::m_ipv6_connection)
            oss << "    a.ipv6,";
        oss << "    a.port, a.player_num,\n"
            << "    a.country_code, a.country_flag, a.country_name, a.version, a.os, a.ping, a.packet_loss,\n"
            << "    b.num_connections, b.first_connected_time, b.first_disconnected_time,\n"
            << "    a.connected_time AS last_connected_time, a.disconnected_time AS last_disconnected_time,\n"
            << "    a.time_played AS last_time_played, b.total_time_played, b.average_time_played,\n"
            << "    b.min_time_played, b.max_time_played\n"
            << "    FROM\n"
            << "    (\n"
            << "        SELECT *,\n"
            << "        ROW_NUMBER() OVER\n"
            << "        (\n"
            << "            PARTITION BY online_id\n"
            << "            ORDER BY connected_time DESC\n"
            << "        ) RowNum\n"
            << "        FROM " << full_stats_view_name << " where online_id != 0\n"
            << "    ) as a\n"
            << "    JOIN\n"
            << "    (\n"
            << "        SELECT online_id, COUNT(online_id) AS num_connections,\n"
            << "        MIN(connected_time) AS first_connected_time,\n"
            << "        MIN(disconnected_time) AS first_disconnected_time,\n"
            << "        ROUND(SUM((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS total_time_played,\n"
            << "        ROUND(AVG((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS average_time_played,\n"
            << "        ROUND(MIN((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS min_time_played,\n"
            << "        ROUND(MAX((STRFTIME(\"%s\", disconnected_time) - STRFTIME(\"%s\", connected_time)) / 60.0), 2) AS max_time_played\n"
            << "        FROM " << m_server_stats_table << " WHERE online_id != 0 GROUP BY online_id\n"
            << "    ) AS b\n"
            << "    ON b.online_id = a.online_id\n"
            << "    WHERE RowNum = 1 ORDER BY num_connections DESC;\n";
    }
    query = oss.str();
    easySQLQuery(query);

    uint32_t last_host_id = 0;
    query = StringUtils::insertValues("SELECT MAX(host_id) FROM %s;",
        m_server_stats_table.c_str());
    ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        ret = sqlite3_step(stmt);
        if (ret == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        {
            last_host_id = (unsigned)sqlite3_column_int64(stmt, 0);
            Log::info("DatabaseConnector", "%u was last server session max host id.",
                last_host_id);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("DatabaseConnector",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
            m_server_stats_table = "";
        }
    }
    else
    {
        Log::error("DatabaseConnector", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        m_server_stats_table = "";
    }
    STKHost::get()->setNextHostId(last_host_id);

    // Update disconnected time (if stk crashed it will not be written)
    query = StringUtils::insertValues(
        "UPDATE %s SET disconnected_time = datetime('now') "
        "WHERE connected_time = disconnected_time;",
        m_server_stats_table.c_str());
    easySQLQuery(query);
}   // initServerStatsTable

//-----------------------------------------------------------------------------
bool DatabaseConnector::writeReport(STKPeer* reporter, std::shared_ptr<NetworkPlayerProfile> reporter_npp,
                 STKPeer* reporting, std::shared_ptr<NetworkPlayerProfile> reporting_npp,
                 irr::core::stringw& info)
{
    std::string query;
    if (ServerConfig::m_ipv6_connection)
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(server_uid, reporter_ip, reporter_ipv6, reporter_online_id, reporter_username, "
            "info, reporting_ip, reporting_ipv6, reporting_online_id, reporting_username) "
            "VALUES (?, %u, \"%s\", %u, ?, ?, %u, \"%s\", %u, ?);",
            ServerConfig::m_player_reports_table.c_str(),
            !reporter->getAddress().isIPv6() ? reporter->getAddress().getIP() : 0,
            reporter->getAddress().isIPv6() ? reporter->getAddress().toString(false) : "",
            reporter_npp->getOnlineId(),
            !reporting->getAddress().isIPv6() ? reporting->getAddress().getIP() : 0,
            reporting->getAddress().isIPv6() ? reporting->getAddress().toString(false) : "",
            reporting_npp->getOnlineId());
    }
    else
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(server_uid, reporter_ip, reporter_online_id, reporter_username, "
            "info, reporting_ip, reporting_online_id, reporting_username) "
            "VALUES (?, %u, %u, ?, ?, %u, %u, ?);",
            ServerConfig::m_player_reports_table.c_str(),
            reporter->getAddress().getIP(), reporter_npp->getOnlineId(),
            reporting->getAddress().getIP(), reporting_npp->getOnlineId());
    }
    return easySQLQuery(query,
        [reporter_npp, reporting_npp, info](sqlite3_stmt* stmt)
    {
        // SQLITE_TRANSIENT to copy string
        if (sqlite3_bind_text(stmt, 1, ServerConfig::m_server_uid.c_str(),
            -1, SQLITE_TRANSIENT) != SQLITE_OK)
        {
            Log::error("easySQLQuery", "Failed to bind %s.",
                ServerConfig::m_server_uid.c_str());
        }
        if (sqlite3_bind_text(stmt, 2,
            StringUtils::wideToUtf8(reporter_npp->getName()).c_str(),
            -1, SQLITE_TRANSIENT) != SQLITE_OK)
        {
            Log::error("easySQLQuery", "Failed to bind %s.",
                StringUtils::wideToUtf8(reporter_npp->getName()).c_str());
        }
        if (sqlite3_bind_text(stmt, 3,
            StringUtils::wideToUtf8(info).c_str(),
            -1, SQLITE_TRANSIENT) != SQLITE_OK)
        {
            Log::error("easySQLQuery", "Failed to bind %s.",
                StringUtils::wideToUtf8(info).c_str());
        }
        if (sqlite3_bind_text(stmt, 4,
            StringUtils::wideToUtf8(reporting_npp->getName()).c_str(),
            -1, SQLITE_TRANSIENT) != SQLITE_OK)
        {
            Log::error("easySQLQuery", "Failed to bind %s.",
                StringUtils::wideToUtf8(reporting_npp->getName()).c_str());
        }
    });
}   // writeReport

//-----------------------------------------------------------------------------
std::vector<DatabaseConnector::IpBanTableData>
DatabaseConnector::getIpBanTableData(uint32_t ip) const
{
    std::vector<IpBanTableData> result;
    if (!m_ip_ban_table_exists)
    {
        return result;
    }
    bool single_ip = (ip != 0);
    std::ostringstream oss;
    oss << "SELECT rowid, ip_start, ip_end, reason, description FROM ";
    oss << (std::string)ServerConfig::m_ip_ban_table << " WHERE ";
    if (single_ip)
        oss << "ip_start <= " << ip << " AND ip_end >= " << ip << " AND ";
    oss << "datetime('now') > datetime(starting_time) AND "
        "(expired_days is NULL OR datetime"
        "(starting_time, '+'||expired_days||' days') > datetime('now'))";
    if (single_ip)
        oss << " LIMIT 1";
    oss << ";";
    std::string query = oss.str();
    sqlite3_exec(m_db, query.c_str(),
        [](void* ptr, int count, char** data, char** columns)
        {
            std::vector<IpBanTableData>* vec = (std::vector<IpBanTableData>*)ptr;
            IpBanTableData element;
            if (!StringUtils::fromString(data[0], element.row_id))
                return 0;
            if (!StringUtils::fromString(data[1], element.ip_start))
                return 0;
            if (!StringUtils::fromString(data[2], element.ip_end))
                return 0;
            element.reason = std::string(data[3]);
            element.description = std::string(data[4]);
            vec->push_back(element);
            return 0;
        }, &result, NULL);
    return result;
}   // getIpBanTableData

//-----------------------------------------------------------------------------
void DatabaseConnector::increaseIpBanTriggerCount(uint32_t ip_start, uint32_t ip_end) const
{
    std::string query = StringUtils::insertValues(
        "UPDATE %s SET trigger_count = trigger_count + 1, "
        "last_trigger = datetime('now') "
        "WHERE ip_start = %u AND ip_end = %u;",
        ServerConfig::m_ip_ban_table.c_str(), ip_start, ip_end);
    easySQLQuery(query);
}   // getIpBanTableData

//-----------------------------------------------------------------------------
std::vector<DatabaseConnector::Ipv6BanTableData>
DatabaseConnector::getIpv6BanTableData(std::string ipv6) const
{
    std::vector<Ipv6BanTableData> result;
    if (!m_ipv6_ban_table_exists)
    {
        return result;
    }
    bool single_ip = !ipv6.empty();
    std::ostringstream oss;
    oss << "SELECT rowid, ipv6_cidr, reason, description FROM ";
    oss << (std::string)ServerConfig::m_ipv6_ban_table;
    oss << " WHERE ";
    if (single_ip)
        oss << "insideIPv6CIDR(ipv6_cidr, ?) = 1 AND ";
    oss << "datetime('now') > datetime(starting_time) AND "
        "(expired_days is NULL OR datetime"
        "(starting_time, '+'||expired_days||' days') > datetime('now'))";
    if (single_ip)
        oss << " LIMIT 1";
    oss << ";";
    std::string query = oss.str();

    sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(m_db, query.c_str(), -1, &stmt, 0);
    if (ret == SQLITE_OK)
    {
        if (single_ip)
        {
            if (sqlite3_bind_text(stmt, 1,
                ipv6.c_str(), -1, SQLITE_TRANSIENT)
                != SQLITE_OK)
            {
                Log::error("DatabaseConnector", "Error binding ipv6 addr for query: %s",
                    sqlite3_errmsg(m_db));
                return result;
            }
        }
        ret = sqlite3_step(stmt);
        while (ret == SQLITE_ROW)
        {
            const char* rowid_cstr = (char*)sqlite3_column_text(stmt, 0);
            const char* ipv6cidr_cstr = (char*)sqlite3_column_text(stmt, 1);
            const char* reason_cstr = (char*)sqlite3_column_text(stmt, 2);
            const char* description_cstr = (char*)sqlite3_column_text(stmt, 3);
            Ipv6BanTableData element;
            if (StringUtils::fromString(rowid_cstr, element.row_id))
            {
                element.ipv6_cidr = std::string(ipv6cidr_cstr);
                element.reason = std::string(reason_cstr);
                element.description = std::string(description_cstr);
                result.push_back(element);
            }
            ret = sqlite3_step(stmt);
        }
        ret = sqlite3_finalize(stmt);
        if (ret != SQLITE_OK)
        {
            Log::error("DatabaseConnector",
                "Error finalize database for query %s: %s",
                query.c_str(), sqlite3_errmsg(m_db));
        }
    }
    else
    {
        Log::error("DatabaseConnector", "Error preparing database for query %s: %s",
            query.c_str(), sqlite3_errmsg(m_db));
        return result;
    }
    return result;
}   // getIpv6BanTableData

//-----------------------------------------------------------------------------
void DatabaseConnector::increaseIpv6BanTriggerCount(const std::string& ipv6_cidr) const
{
    std::string query = StringUtils::insertValues(
        "UPDATE %s SET trigger_count = trigger_count + 1, "
        "last_trigger = datetime('now') "
        "WHERE ipv6_cidr = ?;", ServerConfig::m_ipv6_ban_table.c_str());
    easySQLQuery(query, [ipv6_cidr](sqlite3_stmt* stmt)
    {
        if (sqlite3_bind_text(stmt, 1, ipv6_cidr.c_str(),
            -1, SQLITE_TRANSIENT) != SQLITE_OK)
        {
            Log::error("easySQLQuery", "Failed to bind %s.",
                ipv6_cidr.c_str());
        }
    });
}   // increaseIpv6BanTriggerCount

//-----------------------------------------------------------------------------
std::vector<DatabaseConnector::OnlineIdBanTableData>
DatabaseConnector::getOnlineIdBanTableData(uint32_t online_id) const
{
    std::vector<OnlineIdBanTableData> result;
    if (!m_online_id_ban_table_exists)
    {
        return result;
    }
    bool single_id = (online_id != 0);
    std::ostringstream oss;
    oss << "SELECT rowid, online_id, reason, description FROM ";
    oss << (std::string)ServerConfig::m_online_id_ban_table;
    oss << " WHERE ";
    if (single_id)
        oss << "online_id = " << online_id << " AND ";
    oss << "datetime('now') > datetime(starting_time) AND "
        "(expired_days is NULL OR datetime"
        "(starting_time, '+'||expired_days||' days') > datetime('now'))";
    if (single_id)
        oss << " LIMIT 1";
    oss << ";";
    std::string query = oss.str();
    sqlite3_exec(m_db, query.c_str(),
        [](void* ptr, int count, char** data, char** columns)
        {
            std::vector<OnlineIdBanTableData>* vec = (std::vector<OnlineIdBanTableData>*)ptr;
            OnlineIdBanTableData element;
            if (!StringUtils::fromString(data[0], element.row_id))
                return 0;
            if (!StringUtils::fromString(data[1], element.online_id))
                return 0;
            element.reason = std::string(data[2]);
            element.description = std::string(data[3]);
            vec->push_back(element);
            return 0;
        }, &result, NULL);
    return result;
}   // getOnlineIdBanTableData

//-----------------------------------------------------------------------------
void DatabaseConnector::increaseOnlineIdBanTriggerCount(uint32_t online_id) const
{
    std::string query = StringUtils::insertValues(
        "UPDATE %s SET trigger_count = trigger_count + 1, "
        "last_trigger = datetime('now') "
        "WHERE online_id = %u;",
        ServerConfig::m_online_id_ban_table.c_str(), online_id);
    easySQLQuery(query);
}   // increaseOnlineIdBanTriggerCount

//-----------------------------------------------------------------------------
void DatabaseConnector::clearOldReports()
{
    if (m_player_reports_table_exists &&
        ServerConfig::m_player_reports_expired_days != 0.0f)
    {
        std::string query = StringUtils::insertValues(
            "DELETE FROM %s "
            "WHERE datetime"
            "(reported_time, '+%f days') < datetime('now');",
            ServerConfig::m_player_reports_table.c_str(),
            ServerConfig::m_player_reports_expired_days);
        easySQLQuery(query);
    }
}   // clearOldReports

//-----------------------------------------------------------------------------
void DatabaseConnector::setDisconnectionTimes(std::vector<uint32_t>& present_hosts)
{
    if (!hasServerStatsTable())
        return;
    std::ostringstream oss;
        oss << "UPDATE " << m_server_stats_table
            << "    SET disconnected_time = datetime('now')"
            << "    WHERE connected_time = disconnected_time";
    if (present_hosts.empty())
    {
        oss << ";";
    }
    else
    {
        oss << " AND host_id NOT IN (";
        for (unsigned i = 0; i < present_hosts.size(); i++)
        {
            if (i > 0)
                oss << ",";
            oss << present_hosts[i];
        }
        oss << ");";
    }
    std::string query = oss.str();
    easySQLQuery(query);
}   // setDisconnectionTimes

//-----------------------------------------------------------------------------
void DatabaseConnector::saveAddressToIpBanTable(const SocketAddress& addr)
{
    if (addr.isIPv6() || !m_db || !m_ip_ban_table_exists)
        return;

    std::string query = StringUtils::insertValues(
        "INSERT INTO %s (ip_start, ip_end) "
        "VALUES (%u, %u);",
        ServerConfig::m_ip_ban_table.c_str(), addr.getIP(), addr.getIP());
    easySQLQuery(query);
}   // saveAddressToIpBanTable

//-----------------------------------------------------------------------------
void DatabaseConnector::onPlayerJoinQueries(std::shared_ptr<STKPeer> peer,
        uint32_t online_id, unsigned player_count, const std::string& country_code)
{
    if (m_server_stats_table.empty() || peer->isAIPeer())
        return;
    std::string query;
    if (ServerConfig::m_ipv6_connection && peer->getAddress().isIPv6())
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(host_id, ip, ipv6 ,port, online_id, username, player_num, "
            "country_code, version, os, ping) "
            "VALUES (%u, 0, \"%s\" ,%u, %u, ?, %u, ?, ?, ?, %u);",
            m_server_stats_table.c_str(), peer->getHostId(),
            peer->getAddress().toString(false), peer->getAddress().getPort(),
            online_id, player_count, peer->getAveragePing());
    }
    else
    {
        query = StringUtils::insertValues(
            "INSERT INTO %s "
            "(host_id, ip, port, online_id, username, player_num, "
            "country_code, version, os, ping) "
            "VALUES (%u, %u, %u, %u, ?, %u, ?, ?, ?, %u);",
            m_server_stats_table.c_str(), peer->getHostId(),
            peer->getAddress().getIP(), peer->getAddress().getPort(),
            online_id, player_count, peer->getAveragePing());
    }
    easySQLQuery(query, [peer, country_code](sqlite3_stmt* stmt)
    {
        if (sqlite3_bind_text(stmt, 1, StringUtils::wideToUtf8(
            peer->getPlayerProfiles()[0]->getName()).c_str(),
            -1, SQLITE_TRANSIENT) != SQLITE_OK)
        {
            Log::error("easySQLQuery", "Failed to bind %s.",
                StringUtils::wideToUtf8(
                peer->getPlayerProfiles()[0]->getName()).c_str());
        }
        if (country_code.empty())
        {
            if (sqlite3_bind_null(stmt, 2) != SQLITE_OK)
            {
                Log::error("easySQLQuery",
                    "Failed to bind NULL for country code.");
            }
        }
        else
        {
            if (sqlite3_bind_text(stmt, 2, country_code.c_str(),
                -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                Log::error("easySQLQuery", "Failed to bind country: %s.",
                    country_code.c_str());
            }
        }
        auto version_os =
            StringUtils::extractVersionOS(peer->getUserVersion());
        if (sqlite3_bind_text(stmt, 3, version_os.first.c_str(),
            -1, SQLITE_TRANSIENT) != SQLITE_OK)
        {
            Log::error("easySQLQuery", "Failed to bind %s.",
                version_os.first.c_str());
        }
        if (sqlite3_bind_text(stmt, 4, version_os.second.c_str(),
            -1, SQLITE_TRANSIENT) != SQLITE_OK)
        {
            Log::error("easySQLQuery", "Failed to bind %s.",
                version_os.second.c_str());
        }
    });
}   // onPlayerJoinQueries

//-----------------------------------------------------------------------------
void DatabaseConnector::listBanTable()
{
    if (!m_db)
        return;
    auto printer = [](void* data, int argc, char** argv, char** name)
    {
        for (int i = 0; i < argc; i++)
        {
            std::cout << name[i] << " = " << (argv[i] ? argv[i] : "NULL")
                << "\n";
        }
        std::cout << "\n";
        return 0;
    };
    if (m_ip_ban_table_exists)
    {
        std::string query = "SELECT * FROM ";
        query += ServerConfig::m_ip_ban_table;
        query += ";";
        std::cout << "IP ban list:\n";
        sqlite3_exec(m_db, query.c_str(), printer, NULL, NULL);
    }
    if (m_online_id_ban_table_exists)
    {
        std::string query = "SELECT * FROM ";
        query += ServerConfig::m_online_id_ban_table;
        query += ";";
        std::cout << "Online Id ban list:\n";
        sqlite3_exec(m_db, query.c_str(), printer, NULL, NULL);
    }
}   // listBanTable
#endif // ENABLE_SQLITE3
