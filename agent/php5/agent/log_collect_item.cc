/*
 * Copyright 2017-2018 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "log_collect_item.h"
#include "openrasp_utils.h"
#include "openrasp_ini.h"
#include "openrasp_log.h"
#include "openrasp_agent.h"
#include "openrasp_agent_manager.h"
#include "shared_config_manager.h"
#include "utils/time.h"
#include "third_party/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/prettywriter.h"

namespace openrasp
{
const long LogCollectItem::time_offset = fetch_time_offset();
const std::string LogCollectItem::status_file = ".status.json";

LogCollectItem::LogCollectItem(const std::string name, const std::string url_path, bool collect_enable)
    : name(name),
      url_path(url_path),
      collect_enable(collect_enable)
{
    // update_curr_suffix();
    std::string status_file_abs = get_base_dir_path() + LogCollectItem::status_file;
    if (access(status_file_abs.c_str(), F_OK) == 0)
    {
        std::string status_json;
        if (get_entire_file_content(status_file_abs.c_str(), status_json))
        {
            OpenraspConfig openrasp_config(status_json, OpenraspConfig::FromType::kJson);
            fpos = openrasp_config.Get<int64_t>("fpos");
            st_ino = openrasp_config.Get<int64_t>("st_ino");
            last_post_time = openrasp_config.Get<int64_t>("last_post_time");
            curr_suffix = openrasp_config.Get<std::string>("curr_suffix", curr_suffix);
        }
    }
}

inline std::string LogCollectItem::get_base_dir_path() const
{
    std::string default_slash(1, DEFAULT_SLASH);
    return std::string(openrasp_ini.root_dir) + default_slash + "logs" + default_slash + name + default_slash;
}

inline void LogCollectItem::update_curr_suffix()
{
    curr_suffix = format_time(RaspLoggerEntry::default_log_suffix,
                              strlen(RaspLoggerEntry::default_log_suffix), (long)time(NULL));
}

std::string LogCollectItem::get_active_log_file() const
{
    return get_base_dir_path() + name + ".log." + curr_suffix;
}

void LogCollectItem::open_active_log()
{
    if (!ifs.is_open())
    {
        ifs.open(get_active_log_file(), std::ifstream::binary);
    }
}

void LogCollectItem::determine_fpos()
{
    open_active_log();
    long curr_st_ino = get_active_file_inode();
    if (0 != curr_st_ino && st_ino != curr_st_ino)
    {
        st_ino = curr_st_ino;
        fpos = 0;
    }
    ifs.seekg(fpos);
    if (!ifs.good())
    {
        ifs.clear();
    }
}

long LogCollectItem::get_active_file_inode()
{
    std::string filename = get_active_log_file();
    struct stat sb;
    if (stat(filename.c_str(), &sb) == 0 && (sb.st_mode & S_IFREG) != 0)
    {
        return (long)sb.st_ino;
    }
    return 0;
}

void LogCollectItem::save_status_snapshot() const
{
    rapidjson::StringBuffer s;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(s);

    writer.StartObject();
    writer.Key("curr_suffix");
    writer.String(curr_suffix.c_str());
    writer.Key("last_post_time");
    writer.Int64(last_post_time);
    writer.Key("fpos");
    writer.Int64(fpos);
    writer.Key("st_ino");
    writer.Int64(st_ino);
    writer.EndObject();

    std::string status_file_abs = get_base_dir_path() + LogCollectItem::status_file;
#ifndef _WIN32
    mode_t oldmask = umask(0);
#endif
    write_str_to_file(status_file_abs.c_str(),
                      std::ofstream::in | std::ofstream::out | std::ofstream::trunc,
                      s.GetString(),
                      s.GetSize());
#ifndef _WIN32
    umask(oldmask);
#endif
}

void LogCollectItem::update_fpos()
{
    ifs.clear();
    fpos = ifs.tellg();
}

void LogCollectItem::update_last_post_time()
{
    last_post_time = (long)time(NULL);
}

std::string LogCollectItem::get_cpmplete_url() const
{
    return std::string(openrasp_ini.backend_url) + url_path;
}

bool LogCollectItem::log_content_qualified(const std::string &content)
{
    if (content.empty())
    {
        return false;
    }
    if (nullptr == openrasp_ini.app_id)
    {
        return false;
    }
    std::string app_id_block = "\"app_id\":\"" + std::string(openrasp_ini.app_id) + "\"";
    if (content.find(app_id_block) == std::string::npos)
    {
        return false;
    }
    std::string rasp_id_block = "\"rasp_id\":\"" + scm->get_rasp_id() + "\"";
    if (content.find(rasp_id_block) == std::string::npos)
    {
        return false;
    }
    return true;
}

bool LogCollectItem::get_post_logs(std::string &body)
{
    if (!collect_enable)
    {
        return false;
    }
    std::string line;
    body.push_back('[');
    int count = 0;
    bool qualified_log_found = false;
    while (std::getline(ifs, line) &&
           count < LogAgent::max_post_logs_account)
    {
        if (log_content_qualified(line))
        {
            qualified_log_found = true;
            body.append(line);
            body.push_back(',');
            ++count;
        }
        else
        {
            if (!qualified_log_found)
            {
                update_fpos();
            }
        }
    }
    body.pop_back();
    body.push_back(']');
    if (0 == count)
    {
        body.clear();
        return false;
    }
    return true;
}

bool LogCollectItem::need_rotate() const
{
    long now = (long)time(NULL);
    return !same_day_in_current_timezone(now, last_post_time, LogCollectItem::time_offset);
}

void LogCollectItem::handle_rotate(bool need_rotate)
{
    last_post_time = (long)time(NULL);
    if (need_rotate)
    {
        cleanup_expired_logs();
        clear();
    }
}

void LogCollectItem::clear()
{
    update_curr_suffix();
    if (ifs.is_open())
    {
        ifs.close();
        ifs.clear();
    }
    fpos = 0;
    st_ino = 0;
}

void LogCollectItem::cleanup_expired_logs() const
{
    long log_max_backup = 30;
    if (nullptr != scm && scm->get_log_max_backup() > 0)
    {
        log_max_backup = scm->get_log_max_backup();
    }
    std::vector<std::string> files_tobe_deleted;
    long now = (long)time(NULL);
    std::string tobe_deleted_date_suffix =
        format_time(RaspLoggerEntry::default_log_suffix,
                    strlen(RaspLoggerEntry::default_log_suffix), now - log_max_backup * 24 * 60 * 60);
    openrasp_scandir(get_base_dir_path(), files_tobe_deleted,
                     [this, &tobe_deleted_date_suffix](const char *filename) {
                         return !strncmp(filename, (this->name + ".log.").c_str(), (this->name + ".log.").size()) &&
                                std::string(filename) < (this->name + ".log." + tobe_deleted_date_suffix);
                     },
                     true);
    for (std::string delete_file : files_tobe_deleted)
    {
        unlink(delete_file.c_str());
    }
}

} // namespace openrasp