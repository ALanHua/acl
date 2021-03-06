#include "acl_stdafx.hpp"
#include "acl_cpp/stdlib/log.hpp"
#include "acl_cpp/stdlib/snprintf.hpp"
#include "acl_cpp/stdlib/dbuf_pool.hpp"
#include "acl_cpp/redis/redis_client.hpp"
#include "acl_cpp/redis/redis_cluster.hpp"
#include "acl_cpp/redis/redis_result.hpp"
#include "acl_cpp/redis/redis_pool.hpp"
#include "acl_cpp/redis/redis_command.hpp"
#include "redis_request.hpp"

namespace acl
{

#define INT_LEN		11
#define	LONG_LEN	21

redis_command::redis_command()
: conn_(NULL)
, cluster_(NULL)
, used_(0)
, slice_req_(false)
, request_buf_(NULL)
, request_obj_(NULL)
, argv_size_(0)
, argv_(NULL)
, argv_lens_(NULL)
, slice_res_(false)
, result_(NULL)
{
	pool_ = NEW dbuf_pool(128000);
}


redis_command::redis_command(redis_client* conn)
: conn_(conn)
, cluster_(NULL)
, used_(0)
, slice_req_(false)
, request_buf_(NULL)
, request_obj_(NULL)
, argv_size_(0)
, argv_(NULL)
, argv_lens_(NULL)
, slice_res_(false)
, result_(NULL)
{
	pool_ = NEW dbuf_pool(128000);
}

redis_command::redis_command(redis_cluster* cluster)
: conn_(NULL)
, cluster_(cluster)
, used_(0)
, slice_req_(false)
, request_buf_(NULL)
, request_obj_(NULL)
, argv_size_(0)
, argv_(NULL)
, argv_lens_(NULL)
, slice_res_(false)
, result_(NULL)
{
	pool_ = NEW dbuf_pool(128000);
}

redis_command::~redis_command()
{
	if (argv_ != NULL)
		acl_myfree(argv_);
	if (argv_lens_ != NULL)
		acl_myfree(argv_lens_);
	delete request_buf_;
	delete request_obj_;
	delete pool_;
}

void redis_command::reset()
{
	if (used_ > 0)
	{
		delete pool_;
		pool_ = NEW dbuf_pool();
		result_ = NULL;
	}
}

void redis_command::set_slice_request(bool on)
{
	slice_req_ = on;
}

void redis_command::set_slice_respond(bool on)
{
	slice_res_ = on;
}

void redis_command::set_client(redis_client* conn)
{
	conn_ = conn;
}

void redis_command::set_cluster(redis_cluster* cluster)
{
	cluster_ = cluster;
}

bool redis_command::eof() const
{
	return conn_ == NULL ? false : conn_->eof();
}

void redis_command::argv_space(size_t n)
{
	if (argv_size_ >= n)
		return;
	argv_size_ = n;
	if (argv_ == NULL)
	{
		argv_ = (const char**) acl_mymalloc(n * sizeof(char*));
		argv_lens_ = (size_t*) acl_mymalloc(n * sizeof(size_t));
	}
	else
	{
		argv_ = (const char**) acl_myrealloc(argv_, n * sizeof(char*));
		argv_lens_ = (size_t*) acl_myrealloc(argv_lens_,
			n * sizeof(size_t));
	}
}

/////////////////////////////////////////////////////////////////////////////

size_t redis_command::result_size() const
{
	return result_ ? result_->get_size() : 0;
}

redis_result_t redis_command::result_type() const
{
	return result_ ? result_->get_type() : REDIS_RESULT_UNKOWN;
}

int redis_command::result_number(bool* success /* = NULL */) const
{
	return result_ ? result_->get_integer(success) : 0;
}

long long int redis_command::result_number64(bool* success /* = NULL */) const
{
	return result_ ? result_->get_integer64(success) : 0;
}

const char* redis_command::get_result(size_t i, size_t* len /* = NULL */) const
{
	return result_ ? result_->get(i, len) : NULL;
}

const char* redis_command::result_status() const
{
	return result_ ? result_->get_status() : "";
}

const char* redis_command::result_error() const
{
	return result_ ? result_->get_error() : "";
}

const redis_result* redis_command::result_child(size_t i) const
{
	return result_ ? result_->get_child(i) : NULL;
}

const char* redis_command::result_value(size_t i, size_t* len /* = NULL */) const
{
	if (result_ == NULL || result_->get_type() != REDIS_RESULT_ARRAY)
		return NULL;
	const redis_result* child = result_->get_child(i);
	if (child == NULL)
		return NULL;
	size_t size = child->get_size();
	if (size == 0)
		return NULL;
	if (size == 1)
		return child->get(0, len);

	// 大内存有可能被切片成多个不连续的小内存
	size = child->get_length();
	size++;
	char* buf = (char*) pool_->dbuf_alloc(size);
	size = child->argv_to_string(buf, size);
	if (len)
		*len = size;
	return buf;
}

const redis_result* redis_command::get_result() const
{
	return result_;
}

redis_pool* redis_command::get_conns(redis_cluster* cluster, const char* info)
{
	char* cmd = pool_->dbuf_strdup(info);
	char* slot = strchr(cmd, ' ');
	if (slot == NULL)
		return NULL;
	*slot++ = 0;
	char* addr = strchr(slot, ' ');
	if (addr == NULL)
		return NULL;
	*addr++ = 0;
	if (*addr == 0)
		return NULL;

	// printf(">>>addr: %s, slot: %s\r\n", addr, slot);
	return (redis_pool*) cluster->get(addr);
}

const redis_result* redis_command::run(redis_cluster* cluster, size_t nchildren)
{
	redis_pool* conns = (redis_pool*) cluster->peek();
	if (conns == NULL)
		return NULL;

	redis_client* conn = (redis_client*) conns->peek();
	if (conn == NULL)
		return NULL;

	redis_result_t type;
	int   n = 0;

	while (n++ <= 10)
	{
		if (slice_req_)
			result_ = conn->run(pool_, *request_obj_, nchildren);
		else
			result_ = conn->run(pool_, *request_buf_, nchildren);

		conns->put(conn, !conn->eof());

		if (result_ == NULL)
			return NULL;

		type = result_->get_type();

		if (type == REDIS_RESULT_UNKOWN)
			return NULL;
		if (type != REDIS_RESULT_ERROR)
			return result_;

		const char* ptr = result_->get_error();
		if (ptr == NULL || *ptr == 0)
			return result_;

		if (strncasecmp(ptr, "MOVED", 5) == 0)
		{
			conns = get_conns(cluster_, ptr);
			if (conns == NULL)
				return result_;
			conn = (redis_client*) conns->peek();
			if (conn == NULL)
				return result_;
			reset();
		}
		else if (strncasecmp(ptr, "ASK", 3) == 0)
		{
			conns = get_conns(cluster_, ptr);
			if (conns == NULL)
				return result_;
			conn = (redis_client*) conns->peek();
			if (conn == NULL)
				return result_;
			reset();
		}
		else
			return result_;
	}

	logger_warn("too many cluster redirect: %d", n);
	return NULL;
}

const redis_result* redis_command::run(size_t nchildren /* = 0 */)
{
	used_++;

	if (cluster_ != NULL)
		return run(cluster_, nchildren);
	else if (conn_ != NULL)
	{
		if (slice_req_)
			result_ = conn_->run(pool_, *request_obj_, nchildren);
		else
			result_ = conn_->run(pool_, *request_buf_, nchildren);
		return result_;
	}
	else
		return NULL;
}

/////////////////////////////////////////////////////////////////////////////

int redis_command::get_number(bool* success /* = NULL */)
{
	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_INTEGER)
	{
		if (success)
			*success = false;
		return -1;
	}
	if (success)
		*success = true;
	return result->get_integer();
}

long long int redis_command::get_number64(bool* success /* = NULL */)
{
	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_INTEGER)
	{
		if (success)
			*success = false;
		return -1;
	}
	if (success)
		*success = true;
	return result->get_integer64();
}

int redis_command::get_number(std::vector<int>& out)
{
	out.clear();

	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_ARRAY)
		return -1;

	size_t size;
	const redis_result** children = result->get_children(&size);
	if (children == NULL || size == 0)
		return 0;
	out.reserve(size);

	const redis_result* rr;
	for (size_t i = 0; i < size; i++)
	{
		rr = children[i];
		out.push_back(rr->get_integer());
	}

	return size;
}

int redis_command::get_number64(std::vector<long long int>& out)
{
	out.clear();

	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_ARRAY)
		return -1;

	size_t size;
	const redis_result** children = result->get_children(&size);
	if (children == NULL || size == 0)
		return 0;
	out.reserve(size);

	const redis_result* rr;
	for (size_t i = 0; i < size; i++)
	{
		rr = children[i];
		out.push_back(rr->get_integer64());
	}

	return size;
}

bool redis_command::check_status(const char* success /* = "OK" */)
{
	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_STATUS)
		return false;
	const char* status = result->get_status();
	if (status == NULL || *status == '\0')
		return false;
	else if (success == NULL || strcasecmp(status, success) == 0)
		return true;
	else
		return false;
}

int redis_command::get_status(std::vector<bool>& out)
{
	out.clear();

	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_ARRAY)
		return -1;

	size_t size;
	const redis_result** children = result->get_children(&size);
	if (children == NULL || size == 0)
		return 0;

	out.reserve(size);

	const redis_result* rr;
	for (size_t i = 0; i < size; i++)
	{
		rr = children[i];
		out.push_back(rr->get_integer() > 0 ? true : false);
	}

	return (int) size;
}

const char* redis_command::get_status()
{
	const redis_result* result = run();
	return result == NULL ? NULL : result->get_status();
}

int redis_command::get_string(string& buf)
{
	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_STRING)
		return -1;
	return result->argv_to_string(buf);
}

int redis_command::get_string(string* buf)
{
	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_STRING)
		return -1;
	if (buf == NULL)
		return (int) result->get_length();
	return result->argv_to_string(*buf);
}

int redis_command::get_string(char* buf, size_t size)
{
	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_STRING)
		return -1;
	return result->argv_to_string(buf, size);
}

int redis_command::get_strings(std::vector<string>& out)
{
	out.clear();

	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_ARRAY)
		return -1;

	size_t size;
	const redis_result** children = result->get_children(&size);
	if (children == NULL)
		return 0;

	if (size > 0)
		out.reserve(size);

	const redis_result* rr;
	string buf(4096);

	for (size_t i = 0; i < size; i++)
	{
		rr = children[i];
		if (rr == NULL || rr->get_type() != REDIS_RESULT_STRING)
			out.push_back("");
		else if (rr->get_size() == 0)
			out.push_back("");
		else 
		{
			rr->argv_to_string(buf);
			out.push_back(buf);
			buf.clear();
		}
	}

	return (int) size;
}

int redis_command::get_strings(std::vector<string>* out)
{
	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_ARRAY)
		return -1;
	if (out == NULL)
		return result->get_size();

	out->clear();

	size_t size;
	const redis_result** children = result->get_children(&size);
	if (children == NULL)
		return 0;

	if (size > 0)
		out->reserve(size);

	const redis_result* rr;
	string buf(4096);

	for (size_t i = 0; i < size; i++)
	{
		rr = children[i];
		if (rr == NULL || rr->get_type() != REDIS_RESULT_STRING)
			out->push_back("");
		else if (rr->get_size() == 0)
			out->push_back("");
		else 
		{
			rr->argv_to_string(buf);
			out->push_back(buf);
			buf.clear();
		}
	}

	return (int) size;
}

int redis_command::get_strings(std::map<string, string>& out)
{
	out.clear();

	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_ARRAY)
		return -1;
	if (result->get_size() == 0)
		return 0;

	size_t size;
	const redis_result** children = result->get_children(&size);
	if (children == NULL)
		return -1;
	if (size % 2 != 0)
		return -1;

	string name_buf, value_buf;

	const redis_result* rr;
	for (size_t i = 0; i < size;)
	{
		rr = children[i];
		if (rr->get_type() != REDIS_RESULT_STRING)
		{
			i += 2;
			continue;
		}
		name_buf.clear();
		value_buf.clear();
		rr->argv_to_string(name_buf);
		i++;
		rr->argv_to_string(value_buf);
		i++;
		out[name_buf] = value_buf;
	}

	return (int) out.size();
}

int redis_command::get_strings(std::vector<string>& names,
	std::vector<string>& values)
{
	names.clear();
	values.clear();

	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_ARRAY)
		return -1;
	if (result->get_size() == 0)
		return 0;

	size_t size;
	const redis_result** children = result->get_children(&size);

	if (children == NULL)
		return -1;
	if (size % 2 != 0)
		return -1;

	string buf;
	const redis_result* rr;

	for (size_t i = 0; i < size;)
	{
		rr = children[i];
		if (rr->get_type() != REDIS_RESULT_STRING)
		{
			i += 2;
			continue;
		}
		buf.clear();
		rr->argv_to_string(buf);
		i++;
		names.push_back(buf);

		buf.clear();
		rr->argv_to_string(buf);
		i++;
		values.push_back(buf);
	}

	return (int) names.size();
}

int redis_command::get_strings(std::vector<const char*>& names,
	std::vector<const char*>& values)
{
	names.clear();
	values.clear();

	const redis_result* result = run();
	if (result == NULL || result->get_type() != REDIS_RESULT_ARRAY)
		return -1;
	if (result->get_size() == 0)
		return 0;

	size_t size;
	const redis_result** children = result->get_children(&size);

	if (children == NULL)
		return -1;
	if (size % 2 != 0)
		return -1;

	char* buf;
	size_t len;
	const redis_result* rr;
	std::vector<const redis_result*>::const_iterator cit;

	for (size_t i = 0; i < size;)
	{
		rr = children[i];
		if (rr->get_type() != REDIS_RESULT_STRING)
		{
			i += 2;
			continue;
		}

		len = rr->get_length() + 1;
		buf = (char*) pool_->dbuf_alloc(len);
		rr->argv_to_string(buf, len);
		i++;
		names.push_back(buf);

		len = rr->get_length() + 1;
		buf = (char*) pool_->dbuf_alloc(len);
		rr->argv_to_string(buf, len);
		i++;
		values.push_back(buf);
	}

	return (int) names.size();
}

/////////////////////////////////////////////////////////////////////////////

const redis_result** redis_command::scan_keys(const char* cmd, const char* key,
	int& cursor, size_t& size, const char* pattern, const size_t* count)
{
	size = 0;
	if (cursor < 0)
		return NULL;

	const char* argv[7];
	size_t lens[7];
	size_t argc = 0;

	argv[argc] = cmd;
	lens[argc] = strlen(cmd);
	argc++;

	if (key && *key)
	{
		argv[argc] = key;
		lens[argc] = strlen(key);
		argc++;
	}

	char cursor_s[INT_LEN];
	safe_snprintf(cursor_s, sizeof(cursor_s), "%d", cursor);
	argv[argc] = cursor_s;
	lens[argc] = strlen(cursor_s);
	argc++;

	if (pattern && *pattern)
	{
		argv[argc] = "MATCH";
		lens[argc] = sizeof("MATCH") - 1;
		argc++;

		argv[argc] = pattern;
		lens[argc] = strlen(pattern);
		argc++;
	}

	if (count && *count > 0)
	{
		argv[argc] = "COUNT";
		lens[argc] = sizeof("COUNT") - 1;
		argc++;

		char count_s[LONG_LEN];
		safe_snprintf(count_s, sizeof(count_s), "%lu",
			(unsigned long) count);
		argv[argc] = count_s;
		lens[argc] = strlen(count_s);
		argc++;
	}

	build_request(argc, argv, lens);
	const redis_result* result = run();
	if (result == NULL)
	{
		cursor = -1;
		return NULL;
	}

	if (result->get_size() != 2)
	{
		cursor = -1;
		return NULL;
	}

	const redis_result* rr = result->get_child(0);
	if (rr == NULL)
	{
		cursor = -1;
		return NULL;
	}
	string tmp(128);
	if (rr->argv_to_string(tmp) < 0)
	{
		cursor = -1;
		return NULL;
	}
	cursor = atoi(tmp.c_str());
	if (cursor < 0)
	{
		cursor = -1;
		return NULL;
	}

	rr = result->get_child(1);
	if (rr == NULL)
	{
		cursor = -1;
		return NULL;
	}

	const redis_result** children = rr->get_children(&size);
	if (children == NULL)
	{
		cursor = 0;
		size = 0;
	}

	return children;
}

void redis_command::reset_request()
{
	if (request_buf_)
		request_buf_->clear();
	if (request_obj_)
		request_obj_->reset();
}

void redis_command::build_request(size_t argc, const char* argv[], size_t lens[])
{
	if (slice_req_)
		build_request2(argc, argv, lens);
	else
		build_request1(argc, argv, lens);
}

void redis_command::build_request1(size_t argc, const char* argv[], size_t lens[])
{
	if (request_buf_ == NULL)
		request_buf_ = NEW string(256);
	else
		request_buf_->clear();
	request_buf_->format("*%lu\r\n", (unsigned long) argc);
	for (size_t i = 0; i < argc; i++)
	{
		request_buf_->format_append("$%lu\r\n", (unsigned long) lens[i]);
		request_buf_->append(argv[i], lens[i]);
		request_buf_->append("\r\n");
	}
	//printf("%s", request_buf_->c_str());
}

void redis_command::build_request2(size_t argc, const char* argv[], size_t lens[])
{
	size_t size = 1 + argc * 3;
	if (request_obj_ == NULL)
		request_obj_ = NEW redis_request();
	else
		request_obj_->reset();
	request_obj_->reserve(size);

#define BLEN	32

	char* buf = (char*) pool_->dbuf_alloc(BLEN);
	int  len = safe_snprintf(buf, BLEN, "*%lu\r\n", (unsigned long) argc);
	request_obj_->put(buf, len);

	for (size_t i = 0; i < argc; i++)
	{
		buf = (char*) pool_->dbuf_alloc(BLEN);
		len = safe_snprintf(buf, BLEN, "$%lu\r\n",
			(unsigned long) lens[i]);
		request_obj_->put(buf, len);

		request_obj_->put(argv[i], lens[i]);

		buf = (char*) pool_->dbuf_strdup("\r\n");
		request_obj_->put(buf, 2);
	}
}

//////////////////////////////////////////////////////////////////////////

void redis_command::build(const char* cmd, const char* key,
	const std::map<string, string>& attrs)
{
	argc_ = 1 + attrs.size() * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	std::map<string, string>::const_iterator cit = attrs.begin();
	for (; cit != attrs.end(); ++cit)
	{
		argv_[i] = cit->first.c_str();
		argv_lens_[i] = strlen(argv_[i]);
		i++;

		argv_[i] = cit->second.c_str();
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const std::map<string, const char*>& attrs)
{
	argc_ = 1 + attrs.size() * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	std::map<string, const char*>::const_iterator cit = attrs.begin();
	for (; cit != attrs.end(); ++cit)
	{
		argv_[i] = cit->first.c_str();
		argv_lens_[i] = strlen(argv_[i]);
		i++;

		argv_[i] = cit->second;
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

/////////////////////////////////////////////////////////////////////////////

void redis_command::build(const char* cmd, const char* key,
	const std::map<int, string>& attrs)
{
	argc_ = 1 + attrs.size() * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	std::map<int, string>::const_iterator cit = attrs.begin();
	for (; cit != attrs.end(); ++cit)
	{
		char* tmp = (char*) pool_->dbuf_alloc(INT_LEN);
		(void) safe_snprintf(tmp, INT_LEN, "%d", cit->first);
		argv_[i] = tmp;
		argv_lens_[i] = strlen(argv_[i]);
		i++;

		argv_[i] = cit->second.c_str();
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const std::map<int, const char*>& attrs)
{
	argc_ = 1 + attrs.size() * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	std::map<int, const char*>::const_iterator cit = attrs.begin();
	for (; cit != attrs.end(); ++cit)
	{
		char* tmp = (char*) pool_->dbuf_alloc(INT_LEN);
		(void) safe_snprintf(tmp, INT_LEN, "%d", cit->first);
		argv_[i] = tmp;
		argv_lens_[i] = strlen(argv_[i]);
		i++;

		argv_[i] = cit->second;
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

/////////////////////////////////////////////////////////////////////////////

void redis_command::build(const char* cmd, const char* key,
	const std::vector<string>& names, const std::vector<string>& values)
{
	if (names.size() != values.size())
	{
		logger_fatal("names's size: %lu, values's size: %lu",
			(unsigned long) names.size(),
			(unsigned long) values.size());
	}

	argc_ = 1 + names.size() * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	size_t size = names.size();
	for (size_t j = 0; j < size; j++)
	{
		argv_[i] = names[j].c_str();
		argv_lens_[i] = strlen(argv_[i]);
		i++;

		argv_[i] = values[j].c_str();
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const std::vector<const char*>& names,
	const std::vector<const char*>& values)
{
	if (names.size() != values.size())
	{
		logger_fatal("names's size: %lu, values's size: %lu",
			(unsigned long) names.size(),
			(unsigned long) values.size());
	}

	argc_ = 1 + names.size() * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	size_t size = names.size();
	for (size_t j = 0; j < size; j++)
	{
		argv_[i] = names[j];
		argv_lens_[i] = strlen(argv_[i]);
		i++;

		argv_[i] = values[j];
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

/////////////////////////////////////////////////////////////////////////////

void redis_command::build(const char* cmd, const char* key,
	const std::vector<int>& names, const std::vector<string>& values)
{
	if (names.size() != values.size())
	{
		logger_fatal("names's size: %lu, values's size: %lu",
			(unsigned long) names.size(),
			(unsigned long) values.size());
	}

	argc_ = 1 + names.size() * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	char* buf4int;
	size_t size = names.size();
	for (size_t j = 0; j < size; j++)
	{
		buf4int = (char*) pool_->dbuf_alloc(INT_LEN);
		(void) safe_snprintf(buf4int, INT_LEN, "%d", names[j]);
		argv_[i] = buf4int;
		argv_lens_[i] = strlen(argv_[i]);
		i++;

		argv_[i] = values[j].c_str();
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const std::vector<int>& names, const std::vector<const char*>& values)
{
	if (names.size() != values.size())
	{
		logger_fatal("names's size: %lu, values's size: %lu",
			(unsigned long) names.size(),
			(unsigned long) values.size());
	}

	argc_ = 1 + names.size() * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	char* buf4int;
	size_t size = names.size();
	for (size_t j = 0; j < size; j++)
	{
		buf4int = (char*) pool_->dbuf_alloc(INT_LEN);
		(void) safe_snprintf(buf4int, INT_LEN, "%d", names[j]);
		argv_[i] = buf4int;
		argv_lens_[i] = strlen(argv_[i]);
		i++;

		argv_[i] = values[j];
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

/////////////////////////////////////////////////////////////////////////////

void redis_command::build(const char* cmd, const char* key,
	const char* names[], const char* values[], size_t argc)
{
	argc_ = 1 + argc * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	for (size_t j = 0; j < argc; j++)
	{
		argv_[i] = names[j];
		argv_lens_[i] = strlen(argv_[i]);
		i++;

		argv_[i] = values[j];
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const int names[], const char* values[], size_t argc)
{
	argc_ = 1 + argc * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	char* buf4int;
	for (size_t j = 0; j < argc; j++)
	{
		buf4int = (char*) pool_->dbuf_alloc(INT_LEN);
		(void) safe_snprintf(buf4int, INT_LEN, "%d", names[j]);
		argv_[i] = buf4int;
		argv_lens_[i] = strlen(argv_[i]);
		i++;

		argv_[i] = values[j];
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const char* names[], const size_t names_len[],
	const char* values[], const size_t values_len[], size_t argc)
{
	argc_ = 1 + argc * 2;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	for (size_t j = 0; j < argc; j++)
	{
		argv_[i] = names[j];
		argv_lens_[i] = names_len[j];
		i++;

		argv_[i] = values[j];
		argv_lens_[i] = values_len[j];
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

/////////////////////////////////////////////////////////////////////////////

void redis_command::build(const char* cmd, const char* key,
	const std::vector<string>& names)
{
	size_t argc = names.size();
	argc_ = 1 + argc;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	for (size_t j = 0; j < argc; j++)
	{
		argv_[i] = names[j].c_str();
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const std::vector<const char*>& names)
{
	size_t argc = names.size();
	argc_ = 1 + argc;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	for (size_t j = 0; j < argc; j++)
	{
		argv_[i] = names[j];
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const std::vector<int>& names)
{
	size_t argc = names.size();
	argc_ = 1 + argc;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	char* buf4int;
	for (size_t j = 0; j < argc; j++)
	{
		buf4int = (char*) pool_->dbuf_alloc(INT_LEN);
		safe_snprintf(buf4int, INT_LEN, "%d", names[j]);
		argv_[i] = buf4int;
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const char* names[], size_t argc)
{
	argc_ = 1 + argc;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	for (size_t j = 0; j < argc; j++)
	{
		argv_[i] = names[j];
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const int names[], size_t argc)
{
	argc_ = 1 + argc;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	char* buf4int;
	for (size_t j = 0; j < argc; j++)
	{
		buf4int = (char*) pool_->dbuf_alloc(INT_LEN);
		safe_snprintf(buf4int, INT_LEN, "%d", names[j]);
		argv_[i] = buf4int;
		argv_lens_[i] = strlen(argv_[i]);
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

void redis_command::build(const char* cmd, const char* key,
	const char* names[], const size_t lens[], size_t argc)
{
	argc_ = 1 + argc;
	if (key != NULL)
		argc_++;
	argv_space(argc_);

	size_t i = 0;
	argv_[i] = cmd;
	argv_lens_[i] = strlen(cmd);
	i++;

	if (key != NULL)
	{
		argv_[i] = key;
		argv_lens_[i] = strlen(key);
		i++;
	}

	for (size_t j = 0; j < argc; j++)
	{
		argv_[i] = names[j];
		argv_lens_[i] = lens[j];
		i++;
	}

	build_request(argc_, argv_, argv_lens_);
}

/////////////////////////////////////////////////////////////////////////////

} // namespace acl
