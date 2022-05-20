#include <ngx_http.h>
#include <libpq-fe.h>

#define DEF_PGPORT 5432

ngx_module_t ngx_pq_module;

enum {
    ngx_pq_type_execute = 1 << 0,
    ngx_pq_type_function = 1 << 1,
    ngx_pq_type_location = 1 << 2,
    ngx_pq_type_output = 1 << 3,
    ngx_pq_type_prepare = 1 << 4,
    ngx_pq_type_query = 1 << 5,
    ngx_pq_type_upstream = 1 << 6,
};

typedef enum {
    ngx_pq_output_type_csv = 2,
    ngx_pq_output_type_none = 0,
    ngx_pq_output_type_plain = 3,
    ngx_pq_output_type_value = 1,
} ngx_pq_output_type_t;

typedef struct {
    ngx_int_t index;
    ngx_str_t str;
} ngx_pq_command_t;

typedef struct {
    ngx_flag_t header;
    ngx_flag_t string;
    ngx_int_t index;
    ngx_pq_output_type_t type;
    ngx_str_t null;
    u_char delimiter;
    u_char escape;
    u_char quote;
} ngx_pq_output_t;

typedef struct {
    ngx_array_t arguments;
    ngx_array_t commands;
    ngx_pq_output_t output;
    ngx_uint_t type;
    struct {
        ngx_int_t index;
        uint32_t oid;
    } function;
    struct {
        ngx_int_t index;
        ngx_str_t str;
    } name;
} ngx_pq_query_t;

typedef struct {
    const char *client_encoding;
    const char **keywords;
    const char **values;
    ngx_msec_t timeout;
    ngx_url_t url;
    PGVerbosity verbosity;
} ngx_pq_connect_t;

typedef struct {
    ngx_array_t queries;
    ngx_http_complex_value_t complex;
    ngx_http_upstream_conf_t upstream;
    ngx_pq_connect_t connect;
} ngx_pq_loc_conf_t;

typedef struct {
    ngx_array_t connects;
    ngx_array_t queries;
    ngx_http_upstream_peer_t peer;
    ngx_log_t *log;
} ngx_pq_srv_conf_t;

static void *ngx_pq_create_srv_conf(ngx_conf_t *cf) {
    ngx_pq_srv_conf_t *conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (!conf) return NULL;
    return conf;
}

static void *ngx_pq_create_loc_conf(ngx_conf_t *cf) {
    ngx_pq_loc_conf_t *conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (!conf) return NULL;
    conf->upstream.buffering = NGX_CONF_UNSET;
    conf->upstream.buffer_size = NGX_CONF_UNSET_SIZE;
    conf->upstream.busy_buffers_size_conf = NGX_CONF_UNSET_SIZE;
    conf->upstream.connect_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.ignore_client_abort = NGX_CONF_UNSET;
    conf->upstream.intercept_errors = NGX_CONF_UNSET;
    conf->upstream.next_upstream_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.next_upstream_tries = NGX_CONF_UNSET_UINT;
    conf->upstream.pass_request_body = NGX_CONF_UNSET;
    conf->upstream.preserve_output = 1;
    conf->upstream.read_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.request_buffering = NGX_CONF_UNSET;
    conf->upstream.send_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.socket_keepalive = NGX_CONF_UNSET;
    ngx_str_set(&conf->upstream.module, "pq");
    return conf;
}

static ngx_path_init_t ngx_pq_temp_path = {
    ngx_string("/var/tmp/nginx/pq_temp"), { 1, 2, 0 }
};

static char *ngx_pq_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_pq_loc_conf_t *prev = parent;
    ngx_pq_loc_conf_t *conf = child;
    if (!conf->upstream.upstream) conf->upstream = prev->upstream;
    ngx_conf_merge_bitmask_value(conf->upstream.next_upstream, prev->upstream.next_upstream, NGX_CONF_BITMASK_SET|NGX_HTTP_UPSTREAM_FT_ERROR|NGX_HTTP_UPSTREAM_FT_TIMEOUT);
    ngx_conf_merge_bufs_value(conf->upstream.bufs, prev->upstream.bufs, 8, ngx_pagesize);
    ngx_conf_merge_msec_value(conf->upstream.connect_timeout, prev->upstream.connect_timeout, 60000);
    ngx_conf_merge_msec_value(conf->upstream.next_upstream_timeout, prev->upstream.next_upstream_timeout, 0);
    ngx_conf_merge_msec_value(conf->upstream.read_timeout, prev->upstream.read_timeout, 60000);
    ngx_conf_merge_msec_value(conf->upstream.send_timeout, prev->upstream.send_timeout, 60000);
    ngx_conf_merge_size_value(conf->upstream.buffer_size, prev->upstream.buffer_size, (size_t)ngx_pagesize);
    ngx_conf_merge_size_value(conf->upstream.busy_buffers_size_conf, prev->upstream.busy_buffers_size_conf, NGX_CONF_UNSET_SIZE);
    ngx_conf_merge_uint_value(conf->upstream.next_upstream_tries, prev->upstream.next_upstream_tries, 0);
    ngx_conf_merge_value(conf->upstream.buffering, prev->upstream.buffering, 1);
    ngx_conf_merge_value(conf->upstream.ignore_client_abort, prev->upstream.ignore_client_abort, 0);
    ngx_conf_merge_value(conf->upstream.intercept_errors, prev->upstream.intercept_errors, 0);
    ngx_conf_merge_value(conf->upstream.request_buffering, prev->upstream.request_buffering, 1);
    ngx_conf_merge_value(conf->upstream.pass_request_body, prev->upstream.pass_request_body, 0);
    ngx_conf_merge_value(conf->upstream.socket_keepalive, prev->upstream.socket_keepalive, 0);
    if (conf->upstream.bufs.num < 2) return "there must be at least 2 \"pq_buffers\"";
    size_t size = conf->upstream.buffer_size;
    if (size < conf->upstream.bufs.size) size = conf->upstream.bufs.size;
    conf->upstream.busy_buffers_size = conf->upstream.busy_buffers_size_conf == NGX_CONF_UNSET_SIZE ? 2 * size : conf->upstream.busy_buffers_size_conf;
    if (conf->upstream.busy_buffers_size < size) return "\"pq_busy_buffers_size\" must be equal to or greater than the maximum of the value of \"pq_buffer_size\" and one of the \"pq_buffers\"";
    if (conf->upstream.busy_buffers_size > (conf->upstream.bufs.num - 1) * conf->upstream.bufs.size) return "\"pq_busy_buffers_size\" must be less than the size of all \"pq_buffers\" minus one buffer";
    if (conf->upstream.next_upstream & NGX_HTTP_UPSTREAM_FT_OFF) conf->upstream.next_upstream = NGX_CONF_BITMASK_SET|NGX_HTTP_UPSTREAM_FT_OFF;
    if (ngx_conf_merge_path_value(cf, &conf->upstream.temp_path, prev->upstream.temp_path, &ngx_pq_temp_path) != NGX_OK) return NGX_CONF_ERROR;
    return NGX_CONF_OK;
}

static ngx_http_module_t ngx_pq_ctx = {
    .preconfiguration = NULL,
    .postconfiguration = NULL,
    .create_main_conf = NULL,
    .init_main_conf = NULL,
    .create_srv_conf = ngx_pq_create_srv_conf,
    .merge_srv_conf = NULL,
    .create_loc_conf = ngx_pq_create_loc_conf,
    .merge_loc_conf = ngx_pq_merge_loc_conf
};

static char *ngx_pq_execute_loc_ups_conf(ngx_conf_t *cf, ngx_command_t *cmd, ngx_array_t *queries) {
    ngx_pq_query_t *query;
    if (!queries->elts && ngx_array_init(queries, cf->pool, 1, sizeof(*query)) != NGX_OK) return "ngx_array_init != NGX_OK";
    if (!(query = ngx_array_push(queries))) return "!ngx_array_push";
    ngx_memzero(query, sizeof(*query));
    ngx_str_t *str = cf->args->elts;
    if (str[1].data[0] == '$') {
        str[1].data++;
        str[1].len--;
        if ((query->name.index = ngx_http_get_variable_index(cf, &str[1])) == NGX_ERROR) return "ngx_http_get_variable_index == NGX_ERROR";
    } else query->name.str = str[1];
    query->type = cmd->offset;
    return ngx_pq_argument_output_loc_conf(cf, cmd, query);
}

static char *ngx_pq_execute_loc_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_pq_loc_conf_t *plcf = conf;
    return ngx_pq_execute_loc_ups_conf(cf, cmd, &plcf->queries);
}

static char *ngx_pq_execute_ups_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_pq_srv_conf_t *pscf = conf;
    return ngx_pq_execute_loc_ups_conf(cf, cmd, &pscf->queries);
}

static char *ngx_pq_log_ups_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_pq_srv_conf_t *pscf = conf;
    return ngx_log_set_log(cf, &pscf->log);
}

static char *ngx_pq_pass_loc_server_ups_conf(ngx_conf_t *cf, ngx_pq_connect_t *connect, ngx_http_upstream_server_t *us) {
    ngx_str_t *args = cf->args->elts;
    ngx_str_t conninfo = ngx_null_string;
    static const ngx_conf_enum_t e[] = {
        { ngx_string("default"), PQERRORS_DEFAULT },
        { ngx_string("sqlstate"), PQERRORS_SQLSTATE },
        { ngx_string("terse"), PQERRORS_TERSE },
        { ngx_string("verbose"), PQERRORS_VERBOSE },
        { ngx_null_string, 0 }
    };
    connect->verbosity = PQERRORS_DEFAULT;
    for (ngx_uint_t i = 1; i < cf->args->nelts; i++) {
        if (us) {
            if (args[i].len > sizeof("weight=") - 1 && !ngx_strncmp(args[i].data, (u_char *)"weight=", sizeof("weight=") - 1)) {
                ngx_str_t str = {
                    .len = args[i].len - (sizeof("weight=") - 1),
                    .data = &args[i].data[sizeof("weight=") - 1],
                };
                ngx_int_t n = ngx_atoi(str.data, str.len);
                if (n == NGX_ERROR) return "\"weight\" value must be number";
                if (n <= 0) return "\"weight\" value must be positive";
                us->weight = (ngx_uint_t)n;
                continue;
            }
            if (args[i].len > sizeof("max_conns=") - 1 && !ngx_strncmp(args[i].data, (u_char *)"max_conns=", sizeof("max_conns=") - 1)) {
                ngx_str_t str = {
                    .len = args[i].len - (sizeof("max_conns=") - 1),
                    .data = &args[i].data[sizeof("max_conns=") - 1],
                };
                ngx_int_t n = ngx_atoi(str.data, str.len);
                if (n == NGX_ERROR) return "\"max_conns\" value must be number";
                us->max_conns = (ngx_uint_t)n;
                continue;
            }
            if (args[i].len > sizeof("max_fails=") - 1 && !ngx_strncmp(args[i].data, (u_char *)"max_fails=", sizeof("max_fails=") - 1)) {
                ngx_str_t str = {
                    .len = args[i].len - (sizeof("max_fails=") - 1),
                    .data = &args[i].data[sizeof("max_fails=") - 1],
                };
                ngx_int_t n = ngx_atoi(str.data, str.len);
                if (n == NGX_ERROR) return "\"max_fails\" value must be number";
                us->max_fails = (ngx_uint_t)n;
                continue;
            }
            if (args[i].len > sizeof("fail_timeout=") - 1 && !ngx_strncmp(args[i].data, (u_char *)"fail_timeout=", sizeof("fail_timeout=") - 1)) {
                ngx_str_t str = {
                    .len = args[i].len - (sizeof("fail_timeout=") - 1),
                    .data = &args[i].data[sizeof("fail_timeout=") - 1],
                };
                ngx_int_t n = ngx_parse_time(&str, 1);
                if (n == NGX_ERROR) return "\"fail_timeout\" value must be time";
                us->fail_timeout = (time_t)n;
                continue;
            }
            if (args[i].len == sizeof("backup") - 1 && !ngx_strncmp(args[i].data, (u_char *)"backup", sizeof("backup") - 1)) {
                us->backup = 1;
                continue;
            }
            if (args[i].len == sizeof("down") - 1 && !ngx_strncmp(args[i].data, (u_char *)"down", sizeof("down") - 1)) {
                us->down = 1;
                continue;
            }
        }
        if (args[i].len > sizeof("error_verbosity=") - 1 && !ngx_strncmp(args[i].data, (u_char *)"error_verbosity=", sizeof("error_verbosity=") - 1)) {
            ngx_str_t str = {
                .len = args[i].len - (sizeof("error_verbosity=") - 1),
                .data = &args[i].data[sizeof("error_verbosity=") - 1],
            };
            ngx_uint_t j;
            for (j = 0; e[j].name.len; j++) if (e[j].name.len == str.len && !ngx_strncmp(e[j].name.data, str.data, str.len)) break;
            if (!e[j].name.len) return "\"error_verbosity\" value must be \"default\", \"sqlstate\", \"terse\" or \"verbose\"";
            connect->verbosity = e[j].value;
            continue;
        }
        if (i > 1) conninfo.len++;
        conninfo.len += args[i].len;
    }
    if (!(conninfo.data = ngx_pnalloc(cf->pool, conninfo.len + 1))) return "!ngx_pnalloc";
    u_char *p = conninfo.data;
    for (ngx_uint_t i = 1; i < cf->args->nelts; i++) {
        if (us) {
            if (args[i].len > sizeof("weight=") - 1 && !ngx_strncmp(args[i].data, (u_char *)"weight=", sizeof("weight=") - 1)) continue;
            if (args[i].len > sizeof("max_conns=") - 1 && !ngx_strncmp(args[i].data, (u_char *)"max_conns=", sizeof("max_conns=") - 1)) continue;
            if (args[i].len > sizeof("max_fails=") - 1 && !ngx_strncmp(args[i].data, (u_char *)"max_fails=", sizeof("max_fails=") - 1)) continue;
            if (args[i].len > sizeof("fail_timeout=") - 1 && !ngx_strncmp(args[i].data, (u_char *)"fail_timeout=", sizeof("fail_timeout=") - 1)) continue;
            if (args[i].len == sizeof("backup") - 1 && !ngx_strncmp(args[i].data, (u_char *)"backup", sizeof("backup") - 1)) continue;
            if (args[i].len == sizeof("down") - 1 && !ngx_strncmp(args[i].data, (u_char *)"down", sizeof("down") - 1)) continue;
        }
        if (args[i].len > sizeof("error_verbosity=") - 1 && !ngx_strncmp(args[i].data, (u_char *)"error_verbosity=", sizeof("error_verbosity=") - 1)) continue;
        if (i > 1) *p++ = ' ';
        p = ngx_copy(p, args[i].data, args[i].len);
    }
    *p = '\0';
    char *err;
    PQconninfoOption *opts = PQconninfoParse((const char *)conninfo.data, &err);
    if (!opts) {
        size_t len;
        if (err && (len = ngx_strlen(err))) {
            err[len - 1] = '\0';
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%s", err);
            PQfreemem(err);
            return NGX_CONF_ERROR;
        }
        return "!PQconninfoParse";
    }
    u_char *connect_timeout = NULL;
    u_char *hostaddr = NULL;
    u_char *host = NULL;
    u_char *port = NULL;
    int arg = 0; // hostaddr or host
    arg++; // connect_timeout
    arg++; // fallback_application_name
    for (PQconninfoOption *opt = opts; opt->keyword; opt++) {
        if (!opt->val) continue;
        if (!ngx_strcasecmp((u_char *)opt->keyword, (u_char *)"connect_timeout")) { connect_timeout = (u_char *)opt->val; continue; }
        if (!ngx_strcasecmp((u_char *)opt->keyword, (u_char *)"fallback_application_name")) continue; // !!! discard any fallback_application_name !!!
        if (!ngx_strcasecmp((u_char *)opt->keyword, (u_char *)"hostaddr")) { hostaddr = (u_char *)opt->val; continue; }
        if (!ngx_strcasecmp((u_char *)opt->keyword, (u_char *)"host")) { host = (u_char *)opt->val; continue; }
        if (!ngx_strcasecmp((u_char *)opt->keyword, (u_char *)"port")) port = (u_char *)opt->val; // !!! not continue !!!
        arg++;
    }
    arg++; // last
    if (!connect_timeout) connect->timeout = 60000; else {
        ngx_int_t n = ngx_parse_time(&(ngx_str_t){ngx_strlen(connect_timeout), connect_timeout}, 0);
        if (n == NGX_ERROR) { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "ngx_parse_time == NGX_ERROR"); PQconninfoFree(opts); return NGX_CONF_ERROR; }
        connect->timeout = (ngx_msec_t)n;
    }
    if (hostaddr) {
        connect->url.url.len = ngx_strlen(hostaddr);
        if (!(connect->url.url.data = ngx_pnalloc(cf->pool, connect->url.url.len + 1))) { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "!ngx_pnalloc"); PQconninfoFree(opts); return NGX_CONF_ERROR; }
        (void)ngx_cpystrn(connect->url.url.data, hostaddr, connect->url.url.len + 1);
    } else if (host) {
        connect->url.url.len = ngx_strlen(host);
        if (!(connect->url.url.data = ngx_pnalloc(cf->pool, connect->url.url.len + 1))) { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "!ngx_pnalloc"); PQconninfoFree(opts); return NGX_CONF_ERROR; }
        (void)ngx_cpystrn(connect->url.url.data, host, connect->url.url.len + 1);
    } else {
        ngx_str_set(&connect->url.url, "unix:///run/postgresql");
        host = connect->url.url.data;
    }
    if (!port) connect->url.default_port = DEF_PGPORT; else {
        ngx_int_t n = ngx_atoi(port, ngx_strlen(port));
        if (n == NGX_ERROR) { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "ngx_atoi == NGX_ERROR"); PQconninfoFree(opts); return NGX_CONF_ERROR; }
        connect->url.default_port = (in_port_t)n;
    }
    if (ngx_parse_url(cf->pool, &connect->url) != NGX_OK) {
        if (connect->url.err) { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "ngx_parse_url(%V:%i) != NGX_OK and %s", &connect->url.url, connect->url.default_port, connect->url.err); }
        else { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "ngx_parse_url(%V:%i) != NGX_OK", &connect->url.url, connect->url.default_port); }
        PQconninfoFree(opts);
        return NGX_CONF_ERROR;
    }
    if (us) {
        us->addrs = connect->url.addrs;
        us->naddrs = connect->url.naddrs;
        us->name = connect->url.url;
    }
    if (host && connect->url.family != AF_UNIX) arg++; // host
    arg++;
    if (!(connect->keywords = ngx_pnalloc(cf->pool, arg * sizeof(const char *)))) { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "!ngx_pnalloc"); PQconninfoFree(opts); return NGX_CONF_ERROR; }
    if (!(connect->values = ngx_pnalloc(cf->pool, arg * sizeof(const char *)))) { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "!ngx_pnalloc"); PQconninfoFree(opts); return NGX_CONF_ERROR; }
    arg = 0; // hostaddr or host
    connect->keywords[arg] = connect->url.family == AF_UNIX ? "host" : "hostaddr";
    connect->values[arg] = (const char *)(connect->url.family == AF_UNIX ? host : hostaddr);
    arg++; // connect_timeout
    connect->keywords[arg] = "connect_timeout";
    if (!connect_timeout) connect->values[arg] = "60"; else {
        size_t val_len = ngx_strlen(connect_timeout);
        if (!(connect->values[arg] = ngx_pnalloc(cf->pool, val_len + 1))) { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "!ngx_pnalloc"); PQconninfoFree(opts); return NGX_CONF_ERROR; }
        (void)ngx_cpystrn((u_char *)connect->values[arg], (u_char *)connect_timeout, val_len + 1);
    }
    arg++; // fallback_application_name
    connect->keywords[arg] = "fallback_application_name";
    connect->values[arg] = "nginx";
    for (PQconninfoOption *opt = opts; opt->keyword; opt++) {
        if (!opt->val) continue;
        if (!ngx_strcasecmp((u_char *)opt->keyword, (u_char *)"connect_timeout")) continue;
        if (!ngx_strcasecmp((u_char *)opt->keyword, (u_char *)"fallback_application_name")) continue;
        if (!ngx_strcasecmp((u_char *)opt->keyword, (u_char *)"hostaddr")) continue;
        if (!ngx_strcasecmp((u_char *)opt->keyword, (u_char *)"host") && connect->url.family == AF_UNIX) continue;
        arg++;
        size_t keyword_len = ngx_strlen(opt->keyword);
        if (!(connect->keywords[arg] = ngx_pnalloc(cf->pool, keyword_len + 1))) { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "!ngx_pnalloc"); PQconninfoFree(opts); return NGX_CONF_ERROR; }
        (void)ngx_cpystrn((u_char *)connect->keywords[arg], (u_char *)opt->keyword, keyword_len + 1);
        size_t val_len = ngx_strlen(opt->val);
        if (!(connect->values[arg] = ngx_pnalloc(cf->pool, val_len + 1))) { ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "!ngx_pnalloc"); PQconninfoFree(opts); return NGX_CONF_ERROR; }
        (void)ngx_cpystrn((u_char *)connect->values[arg], (u_char *)opt->val, val_len + 1);
        if (!ngx_strcasecmp((u_char *)opt->keyword, (u_char *)"client_encoding")) connect->client_encoding = connect->values[arg];
    }
    arg++; // last
    connect->keywords[arg] = NULL;
    connect->values[arg] = NULL;
    PQconninfoFree(opts);
    ngx_pfree(cf->pool, conninfo.data);
    return NGX_CONF_OK;
}

static char *ngx_pq_pass_loc_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_pq_loc_conf_t *plcf = conf;
    if (plcf->upstream.upstream || plcf->complex.value.data) return "is duplicate";
    ngx_http_core_loc_conf_t *clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_pq_handler;
    if (clcf->name.data[clcf->name.len - 1] == '/') clcf->auto_redirect = 1;
    ngx_str_t *str = cf->args->elts;
    if (ngx_http_script_variables_count(&str[1])) {
        ngx_http_compile_complex_value_t ccv = {cf, &str[1], &plcf->complex, 0, 0, 0};
        if (ngx_http_compile_complex_value(&ccv) != NGX_OK) return "ngx_http_compile_complex_value != NGX_OK";
        return NGX_CONF_OK;
    }
    if (ngx_pq_pass_loc_server_ups_conf(cf, &plcf->connect, NULL) == NGX_CONF_ERROR) return NGX_CONF_ERROR;
    if (!(plcf->upstream.upstream = ngx_http_upstream_add(cf, &plcf->connect.url, 0))) return NGX_CONF_ERROR;
    ngx_http_upstream_srv_conf_t *uscf = plcf->upstream.upstream;
    uscf->peer.init_upstream = ngx_pq_peer_init_upstream;
    return NGX_CONF_OK;
}

static char *ngx_pq_prepare_query_loc_ups_conf(ngx_conf_t *cf, ngx_command_t *cmd, ngx_array_t *queries) {
    ngx_pq_query_t *query;
    if (!queries->elts && ngx_array_init(queries, cf->pool, 1, sizeof(*query)) != NGX_OK) return "ngx_array_init != NGX_OK";
    if (!(query = ngx_array_push(queries))) return "!ngx_array_push";
    ngx_memzero(query, sizeof(*query));
    ngx_pq_command_t *command;
    if (ngx_array_init(&query->commands, cf->pool, 1, sizeof(*command)) != NGX_OK) return "ngx_array_init != NGX_OK";
    ngx_str_t *str = cf->args->elts;
    ngx_uint_t i = 1;
    query->type = cmd->offset;
    if (cmd->offset & ngx_pq_type_prepare) {
        if (str[i].data[0] == '$') {
            str[i].data++;
            str[i].len--;
            if ((query->name.index = ngx_http_get_variable_index(cf, &str[i])) == NGX_ERROR) return "ngx_http_get_variable_index == NGX_ERROR";
        } else query->name.str = str[i];
        i++;
    }
    u_char *b = str[i].data;
    u_char *e = str[i].data + str[i].len;
    u_char *n = b;
    u_char *s = n;
    while (s < e) {
        if (*s++ == '$') {
            if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_') {
                if (!(command = ngx_array_push(&query->commands))) return "!ngx_array_push";
                ngx_memzero(command, sizeof(*command));
                command->str.data = n;
                command->str.len = s - n - 1;
                n = s;
                while (s < e && ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) s++;
                if (!(command = ngx_array_push(&query->commands))) return "!ngx_array_push";
                ngx_memzero(command, sizeof(*command));
                command->str.data = n;
                command->str.len = s - n;
                n = s;
                if (*s != '$') if ((command->index = ngx_http_get_variable_index(cf, &command->str)) == NGX_ERROR) return "ngx_http_get_variable_index == NGX_ERROR";
            } else {
                if (!(command = ngx_array_push(&query->commands))) return "!ngx_array_push";
                ngx_memzero(command, sizeof(*command));
                command->str.data = n;
                command->str.len = s - n;
                n = s;
            }
        }
    }
    if (n < s) {
        if (!(command = ngx_array_push(&query->commands))) return "!ngx_array_push";
        ngx_memzero(command, sizeof(*command));
        command->str.data = n;
        command->str.len = s - n;
    }
    return ngx_pq_argument_output_loc_conf(cf, cmd, query);
}

static char *ngx_pq_prepare_loc_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_pq_loc_conf_t *plcf = conf;
    return ngx_pq_prepare_query_loc_ups_conf(cf, cmd, &plcf->queries);
}

static char *ngx_pq_prepare_ups_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_pq_srv_conf_t *pscf = conf;
    return ngx_pq_prepare_query_loc_ups_conf(cf, cmd, &pscf->queries);
}

static char *ngx_pq_query_loc_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_pq_loc_conf_t *plcf = conf;
    return ngx_pq_prepare_query_loc_ups_conf(cf, cmd, &plcf->queries);
}

static char *ngx_pq_query_ups_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_pq_srv_conf_t *pscf = conf;
    return ngx_pq_prepare_query_loc_ups_conf(cf, cmd, &pscf->queries);
}

static char *ngx_pq_server_ups_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_pq_srv_conf_t *pscf = conf;
    ngx_http_upstream_srv_conf_t *uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    if (uscf->peer.init_upstream != ngx_pq_peer_init_upstream) {
        pscf->peer.init_upstream = uscf->peer.init_upstream ? uscf->peer.init_upstream : ngx_http_upstream_init_round_robin;
        uscf->peer.init_upstream = ngx_pq_peer_init_upstream;
    }
    ngx_pq_connect_t *connect;
    if (!pscf->connects.nelts && ngx_array_init(&pscf->connects, cf->pool, 1, sizeof(*connect)) != NGX_OK) return "ngx_array_init != NGX_OK";
    if (!(connect = ngx_array_push(&pscf->connects))) return "!ngx_array_push";
    ngx_memzero(connect, sizeof(*connect));
    ngx_http_upstream_server_t *us;
    if (!(us = ngx_array_push(uscf->servers))) return "!ngx_array_push";
    ngx_memzero(us, sizeof(*us));
    us->fail_timeout = 10;
    us->max_fails = 1;
    us->weight = 1;
    return ngx_pq_pass_loc_server_ups_conf(cf, connect, us);
}

static ngx_conf_bitmask_t ngx_pq_next_upstream_masks[] = {
  { ngx_string("error"), NGX_HTTP_UPSTREAM_FT_ERROR },
  { ngx_string("http_403"), NGX_HTTP_UPSTREAM_FT_HTTP_403 },
  { ngx_string("http_404"), NGX_HTTP_UPSTREAM_FT_HTTP_404 },
  { ngx_string("http_429"), NGX_HTTP_UPSTREAM_FT_HTTP_429 },
  { ngx_string("http_500"), NGX_HTTP_UPSTREAM_FT_HTTP_500 },
  { ngx_string("http_503"), NGX_HTTP_UPSTREAM_FT_HTTP_503 },
  { ngx_string("invalid_header"), NGX_HTTP_UPSTREAM_FT_INVALID_HEADER },
  { ngx_string("non_idempotent"), NGX_HTTP_UPSTREAM_FT_NON_IDEMPOTENT },
  { ngx_string("off"), NGX_HTTP_UPSTREAM_FT_OFF },
  { ngx_string("timeout"), NGX_HTTP_UPSTREAM_FT_TIMEOUT },
  { ngx_string("updating"), NGX_HTTP_UPSTREAM_FT_UPDATING },
  { ngx_null_string, 0 }
};

static ngx_command_t ngx_pq_commands[] = {
  { ngx_string("pq_execute"), NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_1MORE, ngx_pq_execute_loc_conf, NGX_HTTP_LOC_CONF_OFFSET, ngx_pq_type_location|ngx_pq_type_execute|ngx_pq_type_output, NULL },
  { ngx_string("pq_execute"), NGX_HTTP_UPS_CONF|NGX_CONF_1MORE, ngx_pq_execute_ups_conf, NGX_HTTP_SRV_CONF_OFFSET, ngx_pq_type_upstream|ngx_pq_type_execute, NULL },
  { ngx_string("pq_log"), NGX_HTTP_UPS_CONF|NGX_CONF_1MORE, ngx_pq_log_ups_conf, NGX_HTTP_SRV_CONF_OFFSET, 0, NULL },
  { ngx_string("pq_pass"), NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_1MORE, ngx_pq_pass_loc_conf, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
  { ngx_string("pq_prepare"), NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_1MORE, ngx_pq_prepare_loc_conf, NGX_HTTP_LOC_CONF_OFFSET, ngx_pq_type_location|ngx_pq_type_prepare, NULL },
  { ngx_string("pq_prepare"), NGX_HTTP_UPS_CONF|NGX_CONF_1MORE, ngx_pq_prepare_ups_conf, NGX_HTTP_SRV_CONF_OFFSET, ngx_pq_type_upstream|ngx_pq_type_prepare, NULL },
  { ngx_string("pq_query"), NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_1MORE, ngx_pq_query_loc_conf, NGX_HTTP_LOC_CONF_OFFSET, ngx_pq_type_location|ngx_pq_type_query|ngx_pq_type_output, NULL },
  { ngx_string("pq_query"), NGX_HTTP_UPS_CONF|NGX_CONF_1MORE, ngx_pq_query_ups_conf, NGX_HTTP_SRV_CONF_OFFSET, ngx_pq_type_upstream|ngx_pq_type_query, NULL },
  { ngx_string("pq_server"), NGX_HTTP_UPS_CONF|NGX_CONF_1MORE, ngx_pq_server_ups_conf, NGX_HTTP_SRV_CONF_OFFSET, 0, NULL },
  { ngx_string("pq_buffering"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG, ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.buffering), NULL },
  { ngx_string("pq_buffer_size"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1, ngx_conf_set_size_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.buffer_size), NULL },
  { ngx_string("pq_buffers"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2, ngx_conf_set_bufs_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.bufs), NULL },
  { ngx_string("pq_busy_buffers_size"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1, ngx_conf_set_size_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.busy_buffers_size_conf), NULL },
  { ngx_string("pq_connect_timeout"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1, ngx_conf_set_msec_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.connect_timeout), NULL },
  { ngx_string("pq_ignore_client_abort"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG, ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.ignore_client_abort), NULL },
  { ngx_string("pq_intercept_errors"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG, ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.intercept_errors), NULL },
  { ngx_string("pq_next_upstream"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE, ngx_conf_set_bitmask_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.next_upstream), &ngx_pq_next_upstream_masks },
  { ngx_string("pq_next_upstream_timeout"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1, ngx_conf_set_msec_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.next_upstream_timeout), NULL },
  { ngx_string("pq_next_upstream_tries"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1, ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.next_upstream_tries), NULL },
  { ngx_string("pq_pass_request_body"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG, ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.pass_request_body), NULL },
  { ngx_string("pq_read_timeout"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1, ngx_conf_set_msec_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.read_timeout), NULL },
  { ngx_string("pq_request_buffering"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG, ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.request_buffering), NULL },
  { ngx_string("pq_send_timeout"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1, ngx_conf_set_msec_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.send_timeout), NULL },
  { ngx_string("pq_socket_keepalive"), NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG, ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ngx_pq_loc_conf_t, upstream.socket_keepalive), NULL },
    ngx_null_command
};

ngx_module_t ngx_pq_module = {
    NGX_MODULE_V1,
    .ctx = &ngx_pq_ctx,
    .commands = ngx_pq_commands,
    .type = NGX_HTTP_MODULE,
    .init_master = NULL,
    .init_module = NULL,
    .init_process = NULL,
    .init_thread = NULL,
    .exit_thread = NULL,
    .exit_process = NULL,
    .exit_master = NULL,
    NGX_MODULE_V1_PADDING
};
