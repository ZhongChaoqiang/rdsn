/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 *
 * -=- Robust Distributed System Nucleus (rDSN) -=-
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Description:
 *     replica interface, the base object which rdsn replicates
 *
 * Revision history:
 *     Mar., 2015, @imzhenyu (Zhenyu Guo), first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */

#pragma once

//
// a replica is a replication partition of a serivce,
// which handles all replication related issues
// and on_request the app messages to replication_app_base
// which is binded to this replication partition
//

#include <dsn/tool-api/uniq_timestamp_us.h>
#include <dsn/tool-api/thread_access_checker.h>
#include <dsn/cpp/serverlet.h>

#include <dsn/perf_counter/perf_counter_wrapper.h>
#include <dsn/dist/replication/replica_base.h>

#include "common/replication_common.h"
#include "mutation.h"
#include "mutation_log.h"
#include "prepare_list.h"
#include "replica_context.h"
#include "utils/throttling_controller.h"

namespace dsn {
namespace replication {

class replication_app_base;
class replica_stub;
class replica_duplicator_manager;
class replica_backup_manager;
class replica_bulk_loader;

class cold_backup_context;
typedef dsn::ref_ptr<cold_backup_context> cold_backup_context_ptr;
class cold_backup_metadata;

namespace test {
class test_checker;
}

class replica : public serverlet<replica>, public ref_counter, public replica_base
{
public:
    ~replica(void);

    //
    //    routines for replica stub
    //
    static replica *load(replica_stub *stub, const char *dir);
    // {parent_dir} is used in partition split for get_child_dir in replica_stub
    static replica *newr(replica_stub *stub,
                         gpid gpid,
                         const app_info &app,
                         bool restore_if_necessary,
                         const std::string &parent_dir = "");

    // return true when the mutation is valid for the current replica
    bool replay_mutation(mutation_ptr &mu, bool is_private);
    void reset_prepare_list_after_replay();

    // return false when update fails or replica is going to be closed
    bool update_local_configuration_with_no_ballot_change(partition_status::type status);
    void set_inactive_state_transient(bool t);
    void check_state_completeness();
    // error_code check_and_fix_private_log_completeness();

    // close() will wait all traced tasks to finish
    void close();

    //
    //    requests from clients
    //
    void on_client_write(message_ex *request, bool ignore_throttling = false);
    void on_client_read(message_ex *request);

    //
    //    Throttling
    //

    /// throttle write requests
    /// \return true if request is throttled.
    /// \see replica::on_client_write
    bool throttle_request(throttling_controller &c, message_ex *request, int32_t req_units);
    /// update throttling controllers
    /// \see replica::update_app_envs
    void update_throttle_envs(const std::map<std::string, std::string> &envs);
    void update_throttle_env_internal(const std::map<std::string, std::string> &envs,
                                      const std::string &key,
                                      throttling_controller &cntl);

    //
    //    messages and tools from/for meta server
    //
    void on_config_proposal(configuration_update_request &proposal);
    void on_config_sync(const app_info &info, const partition_configuration &config);
    void on_cold_backup(const backup_request &request, /*out*/ backup_response &response);

    //
    //    messages from peers (primary or secondary)
    //
    void on_prepare(dsn::message_ex *request);
    void on_learn(dsn::message_ex *msg, const learn_request &request);
    void on_learn_completion_notification(const group_check_response &report,
                                          /*out*/ learn_notify_response &response);
    void on_learn_completion_notification_reply(error_code err,
                                                group_check_response &&report,
                                                learn_notify_response &&resp);
    void on_add_learner(const group_check_request &request);
    void on_remove(const replica_configuration &request);
    void on_group_check(const group_check_request &request, /*out*/ group_check_response &response);
    void on_copy_checkpoint(const replica_configuration &request, /*out*/ learn_response &response);

    //
    //    messsages from liveness monitor
    //
    void on_meta_server_disconnected();

    //
    //  routine for testing purpose only
    //
    void inject_error(error_code err);

    //
    //  local information query
    //
    ballot get_ballot() const { return _config.ballot; }
    partition_status::type status() const { return _config.status; }
    replication_app_base *get_app() { return _app.get(); }
    const app_info *get_app_info() const { return &_app_info; }
    decree max_prepared_decree() const { return _prepare_list->max_decree(); }
    decree last_committed_decree() const { return _prepare_list->last_committed_decree(); }
    decree last_prepared_decree() const;
    decree last_durable_decree() const;
    decree last_flushed_decree() const;
    const std::string &dir() const { return _dir; }
    uint64_t create_time_milliseconds() const { return _create_time_ms; }
    const char *name() const { return replica_name(); }
    mutation_log_ptr private_log() const { return _private_log; }
    const replication_options *options() const { return _options; }
    replica_stub *get_replica_stub() { return _stub; }
    bool verbose_commit_log() const;
    dsn::task_tracker *tracker() { return &_tracker; }

    //
    // Duplication
    //
    replica_duplicator_manager *get_duplication_manager() const { return _duplication_mgr.get(); }
    bool is_duplicating() const { return _duplicating; }

    //
    // Backup
    //
    replica_backup_manager *get_backup_manager() const { return _backup_mgr.get(); }

    void update_last_checkpoint_generate_time();

    //
    // Bulk load
    //
    replica_bulk_loader *get_bulk_loader() const { return _bulk_loader.get(); }
    inline uint64_t ingestion_duration_ms() const
    {
        return _bulk_load_ingestion_start_time_ms > 0
                   ? (dsn_now_ms() - _bulk_load_ingestion_start_time_ms)
                   : 0;
    }

    //
    // Statistics
    //
    void update_commit_qps(int count);

    // routine for get extra envs from replica
    const std::map<std::string, std::string> &get_replica_extra_envs() const { return _extra_envs; }

protected:
    // this method is marked protected to enable us to mock it in unit tests.
    virtual decree max_gced_decree_no_lock() const;

private:
    // common helpers
    void init_state();
    void response_client_read(dsn::message_ex *request, error_code error);
    void response_client_write(dsn::message_ex *request, error_code error);
    void execute_mutation(mutation_ptr &mu);
    mutation_ptr new_mutation(decree decree);

    // initialization
    replica(replica_stub *stub, gpid gpid, const app_info &app, const char *dir, bool need_restore);
    error_code initialize_on_new();
    error_code initialize_on_load();
    error_code init_app_and_prepare_list(bool create_new);
    decree get_replay_start_decree();

    /////////////////////////////////////////////////////////////////
    // 2pc
    // `pop_all_committed_mutations = true` will be used for ingestion empty write
    // See more about it in `replica_bulk_loader.cpp`
    void
    init_prepare(mutation_ptr &mu, bool reconciliation, bool pop_all_committed_mutations = false);
    void send_prepare_message(::dsn::rpc_address addr,
                              partition_status::type status,
                              const mutation_ptr &mu,
                              int timeout_milliseconds,
                              bool pop_all_committed_mutations = false,
                              int64_t learn_signature = invalid_signature);
    void on_append_log_completed(mutation_ptr &mu, error_code err, size_t size);
    void on_prepare_reply(std::pair<mutation_ptr, partition_status::type> pr,
                          error_code err,
                          dsn::message_ex *request,
                          dsn::message_ex *reply);
    void do_possible_commit_on_primary(mutation_ptr &mu);
    void ack_prepare_message(error_code err, mutation_ptr &mu);
    void cleanup_preparing_mutations(bool wait);

    /////////////////////////////////////////////////////////////////
    // learning
    void init_learn(uint64_t signature);
    void on_learn_reply(error_code err, learn_request &&req, learn_response &&resp);
    void on_copy_remote_state_completed(error_code err,
                                        size_t size,
                                        uint64_t copy_start_time,
                                        learn_request &&req,
                                        learn_response &&resp);
    void on_learn_remote_state_completed(error_code err);
    void handle_learning_error(error_code err, bool is_local_error);
    error_code handle_learning_succeeded_on_primary(::dsn::rpc_address node,
                                                    uint64_t learn_signature);
    void notify_learn_completion();
    error_code apply_learned_state_from_private_log(learn_state &state);

    // Gets the position where this round of the learning process should begin.
    // This method is called on primary-side.
    // TODO(wutao1): mark it const
    decree get_learn_start_decree(const learn_request &req);

    // This method differs with `_private_log->max_gced_decree()` in that
    // it also takes `learn/` dir into account, since the learned logs are
    // a part of plog as well.
    // This method is called on learner-side.
    decree get_max_gced_decree_for_learn() const;

    /////////////////////////////////////////////////////////////////
    // failure handling
    void handle_local_failure(error_code error);
    void handle_remote_failure(partition_status::type status,
                               ::dsn::rpc_address node,
                               error_code error,
                               const std::string &caused_by);

    /////////////////////////////////////////////////////////////////
    // reconfiguration
    void assign_primary(configuration_update_request &proposal);
    void add_potential_secondary(configuration_update_request &proposal);
    void upgrade_to_secondary_on_primary(::dsn::rpc_address node);
    void downgrade_to_secondary_on_primary(configuration_update_request &proposal);
    void downgrade_to_inactive_on_primary(configuration_update_request &proposal);
    void remove(configuration_update_request &proposal);
    void update_configuration_on_meta_server(config_type::type type,
                                             ::dsn::rpc_address node,
                                             partition_configuration &newConfig);
    void
    on_update_configuration_on_meta_server_reply(error_code err,
                                                 dsn::message_ex *request,
                                                 dsn::message_ex *response,
                                                 std::shared_ptr<configuration_update_request> req);
    void replay_prepare_list();
    bool is_same_ballot_status_change_allowed(partition_status::type olds,
                                              partition_status::type news);

    void update_app_envs(const std::map<std::string, std::string> &envs);
    void update_app_envs_internal(const std::map<std::string, std::string> &envs);
    void query_app_envs(/*out*/ std::map<std::string, std::string> &envs);

    bool update_configuration(const partition_configuration &config);
    bool update_local_configuration(const replica_configuration &config, bool same_ballot = false);

    /////////////////////////////////////////////////////////////////
    // group check
    void init_group_check();
    void broadcast_group_check();
    void on_group_check_reply(error_code err,
                              const std::shared_ptr<group_check_request> &req,
                              const std::shared_ptr<group_check_response> &resp);

    /////////////////////////////////////////////////////////////////
    // check timer for gc, checkpointing etc.
    void on_checkpoint_timer();
    void init_checkpoint(bool is_emergency);
    error_code background_async_checkpoint(bool is_emergency);
    error_code background_sync_checkpoint();
    void catch_up_with_private_logs(partition_status::type s);
    void on_checkpoint_completed(error_code err);
    void on_copy_checkpoint_ack(error_code err,
                                const std::shared_ptr<replica_configuration> &req,
                                const std::shared_ptr<learn_response> &resp);
    void on_copy_checkpoint_file_completed(error_code err,
                                           size_t sz,
                                           std::shared_ptr<learn_response> resp,
                                           const std::string &chk_dir);

    /////////////////////////////////////////////////////////////////
    // cold backup
    void generate_backup_checkpoint(cold_backup_context_ptr backup_context);
    void trigger_async_checkpoint_for_backup(cold_backup_context_ptr backup_context);
    void wait_async_checkpoint_for_backup(cold_backup_context_ptr backup_context);
    void local_create_backup_checkpoint(cold_backup_context_ptr backup_context);
    void send_backup_request_to_secondary(const backup_request &request);
    // set all cold_backup_state cancel/pause
    void set_backup_context_cancel();
    void clear_cold_backup_state();

    /////////////////////////////////////////////////////////////////
    // replica restore from backup
    bool read_cold_backup_metadata(const std::string &file, cold_backup_metadata &backup_metadata);
    // checkpoint on cold backup media maybe contain useless file,
    // we should abandon these file base cold_backup_metadata
    bool remove_useless_file_under_chkpt(const std::string &chkpt_dir,
                                         const cold_backup_metadata &metadata);
    void clear_restore_useless_files(const std::string &local_chkpt_dir,
                                     const cold_backup_metadata &metadata);
    error_code get_backup_metadata(dist::block_service::block_filesystem *fs,
                                   const std::string &remote_chkpt_dir,
                                   const std::string &local_chkpt_dir,
                                   cold_backup_metadata &backup_metadata);
    error_code download_checkpoint(const configuration_restore_request &req,
                                   const std::string &remote_chkpt_dir,
                                   const std::string &local_chkpt_dir);
    dsn::error_code find_valid_checkpoint(const configuration_restore_request &req,
                                          /*out*/ std::string &remote_chkpt_dir);
    dsn::error_code restore_checkpoint();

    dsn::error_code skip_restore_partition(const std::string &restore_dir);
    void tell_meta_to_restore_rollback();

    void report_restore_status_to_meta();

    void update_restore_progress(uint64_t f_size);

    std::string query_compact_state() const;

    /////////////////////////////////////////////////////////////////
    // partition split
    // parent partition create child
    void on_add_child(const group_check_request &request);

    // child replica initialize config and state info
    void child_init_replica(gpid parent_gpid, dsn::rpc_address primary_address, ballot init_ballot);

    void parent_prepare_states(const std::string &dir);

    // child copy parent prepare list and call child_learn_states
    void child_copy_prepare_list(learn_state lstate,
                                 std::vector<mutation_ptr> mutation_list,
                                 std::vector<std::string> plog_files,
                                 uint64_t total_file_size,
                                 std::shared_ptr<prepare_list> plist);

    // child learn states(including checkpoint, private logs, in-memory mutations)
    void child_learn_states(learn_state lstate,
                            std::vector<mutation_ptr> mutation_list,
                            std::vector<std::string> plog_files,
                            uint64_t total_file_size,
                            decree last_committed_decree);

    // TODO(heyuchen): total_file_size is used for split perf-counter in further pull request
    // Applies mutation logs that were learned from the parent of this child.
    // This stage follows after that child applies the checkpoint of parent, and begins to apply the
    // mutations.
    // \param last_committed_decree: parent's last_committed_decree when the checkpoint was
    // generated.
    error_code child_apply_private_logs(std::vector<std::string> plog_files,
                                        std::vector<mutation_ptr> mutation_list,
                                        uint64_t total_file_size,
                                        decree last_committed_decree);

    // child catch up parent states while executing async learn task
    void child_catch_up_states();

    // child send notification to primary parent when it finish async learn
    void child_notify_catch_up();

    // primary parent handle child catch_up request
    void parent_handle_child_catch_up(const notify_catch_up_request &request,
                                      notify_cacth_up_response &response);

    // primary parent check if sync_point has been committed
    // sync_point is the first decree after parent send write request to child synchronously
    void parent_check_sync_point_commit(decree sync_point);

    // primary parent register children on meta_server
    void register_child_on_meta(ballot b);
    void on_register_child_on_meta_reply(dsn::error_code ec,
                                         const register_child_request &request,
                                         const register_child_response &response);
    // primary sends register request to meta_server
    void parent_send_register_request(const register_child_request &request);

    // child partition has been registered on meta_server, could be active
    void child_partition_active(const partition_configuration &config);

    // return true if parent status is valid
    bool parent_check_states();

    // parent reset child information when partition split failed
    void parent_cleanup_split_context();
    // child suicide when partition split failed
    void child_handle_split_error(const std::string &error_msg);
    // child handle error while async learn parent states
    void child_handle_async_learn_error();

    void init_table_level_latency_counters();

private:
    friend class ::dsn::replication::test::test_checker;
    friend class ::dsn::replication::mutation_queue;
    friend class ::dsn::replication::replica_stub;
    friend class mock_replica;
    friend class throttling_controller_test;
    friend class replica_learn_test;
    friend class replica_duplicator_manager;
    friend class load_mutation;
    friend class replica_split_test;
    friend class replica_test;
    friend class replica_backup_manager;
    friend class replica_bulk_loader;

    // replica configuration, updated by update_local_configuration ONLY
    replica_configuration _config;
    uint64_t _create_time_ms;
    uint64_t _last_config_change_time_ms;
    uint64_t _last_checkpoint_generate_time_ms;
    uint64_t _next_checkpoint_interval_trigger_time_ms;

    // prepare list
    prepare_list *_prepare_list;

    // private prepare log (may be empty, depending on config)
    mutation_log_ptr _private_log;

    // local checkpoint timer for gc, checkpoint, etc.
    dsn::task_ptr _checkpoint_timer;

    // application
    std::unique_ptr<replication_app_base> _app;

    // constants
    replica_stub *_stub;
    std::string _dir;
    replication_options *_options;
    const app_info _app_info;
    std::map<std::string, std::string> _extra_envs;

    // uniq timestamp generator for this replica.
    //
    // we use it to generate an increasing timestamp for current replica
    // and replicate it to secondary in preparing mutations, and secodaries'
    // timestamp value will also updated if value from primary is larger
    //
    // as the timestamp is recorded in mutation log with mutations, we also update the value
    // when do replaying
    //
    // in addition, as a replica can only be accessed by one thread,
    // so the "thread-unsafe" generator works fine
    uniq_timestamp_us _uniq_timestamp_us;

    // replica status specific states
    primary_context _primary_states;
    secondary_context _secondary_states;
    potential_secondary_context _potential_secondary_states;
    // policy_name --> cold_backup_context
    std::map<std::string, cold_backup_context_ptr> _cold_backup_contexts;
    partition_split_context _split_states;

    // timer task that running in replication-thread
    std::atomic<uint64_t> _cold_backup_running_count;
    std::atomic<uint64_t> _cold_backup_max_duration_time_ms;
    std::atomic<uint64_t> _cold_backup_max_upload_file_size;

    // record the progress of restore
    int64_t _chkpt_total_size;
    std::atomic<int64_t> _cur_download_size;
    std::atomic<int32_t> _restore_progress;
    // _restore_status:
    //      ERR_OK: restore haven't encounter some error
    //      ERR_CORRUPTION : data on backup media is damaged and we can not skip the damage data,
    //                       so should restore rollback
    //      ERR_IGNORE_DAMAGED_DATA : data on backup media is damaged but we can skip the damage
    //                                data, so skip the damaged partition
    dsn::error_code _restore_status;

    bool _inactive_is_transient; // upgrade to P/S is allowed only iff true
    bool _is_initializing;       // when initializing, switching to primary need to update ballot
    bool _deny_client_write;     // if deny all write requests
    throttling_controller _write_qps_throttling_controller;  // throttling by requests-per-second
    throttling_controller _write_size_throttling_controller; // throttling by bytes-per-second

    // duplication
    std::unique_ptr<replica_duplicator_manager> _duplication_mgr;
    bool _duplicating{false};

    // backup
    std::unique_ptr<replica_backup_manager> _backup_mgr;

    // partition split
    // _child_gpid = gpid({app_id},{pidx}+{old_partition_count}) for parent partition
    // _child_gpid.app_id = 0 for parent partition not in partition split and child partition
    dsn::gpid _child_gpid{0, 0};
    // ballot when starting partition split and split will stop if ballot changed
    // _child_init_ballot = 0 if partition not in partition split
    ballot _child_init_ballot{0};
    // in normal cases, _partition_version = partition_count-1
    // when replica reject client read write request, partition_version = -1
    std::atomic<int32_t> _partition_version;

    // bulk load
    std::unique_ptr<replica_bulk_loader> _bulk_loader;
    // if replica in bulk load ingestion 2pc, will reject other write requests
    bool _is_bulk_load_ingestion{false};
    uint64_t _bulk_load_ingestion_start_time_ms{0};

    // perf counters
    perf_counter_wrapper _counter_private_log_size;
    perf_counter_wrapper _counter_recent_write_throttling_delay_count;
    perf_counter_wrapper _counter_recent_write_throttling_reject_count;
    std::vector<perf_counter *> _counters_table_level_latency;
    perf_counter_wrapper _counter_dup_disabled_non_idempotent_write_count;
    perf_counter_wrapper _counter_backup_request_qps;

    dsn::task_tracker _tracker;
    // the thread access checker
    dsn::thread_access_checker _checker;
};
typedef dsn::ref_ptr<replica> replica_ptr;
} // namespace replication
} // namespace dsn
