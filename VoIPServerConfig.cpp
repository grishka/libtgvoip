//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include "VoIPServerConfig.h"
#include <stdlib.h>
#include "logging.h"
#include <sstream>
#include <locale>
#include <utility>
#include "threading.h"
#include "json11.hpp"

using namespace tgvoip;

namespace tgvoip {
class ServerConfigImpl {
public:
	int32_t GetInt(std::string name, int32_t fallback);
	double GetDouble(std::string name, double fallback);
	std::string GetString(std::string name, std::string fallback);
	bool GetBoolean(std::string name, bool fallback);
	void Update(std::string jsonString);

private:
	bool ContainsKey(std::string key);
	json11::Json config;
	Mutex mutex;
};
}

namespace{
ServerConfig* sharedInstance=NULL;
}

bool ServerConfigImpl::GetBoolean(std::string name, bool fallback){
	MutexGuard sync(mutex);
	if(ContainsKey(name) && config[name].is_bool())
		return config[name].bool_value();
	return fallback;
}

double ServerConfigImpl::GetDouble(std::string name, double fallback){
	MutexGuard sync(mutex);
	if(ContainsKey(name) && config[name].is_number())
		return config[name].number_value();
	return fallback;
}

int32_t ServerConfigImpl::GetInt(std::string name, int32_t fallback){
	MutexGuard sync(mutex);
	if(ContainsKey(name) && config[name].is_number())
		return config[name].int_value();
	return fallback;
}

std::string ServerConfigImpl::GetString(std::string name, std::string fallback){
	MutexGuard sync(mutex);
	if(ContainsKey(name) && config[name].is_string())
		return config[name].string_value();
	return fallback;
}

void ServerConfigImpl::Update(std::string jsonString){
	MutexGuard sync(mutex);
	LOGD("=== Updating voip config ===");
	LOGD("%s", jsonString.c_str());
	std::string jsonError;
	config=json11::Json::parse(jsonString, jsonError);
	if(!jsonError.empty())
		LOGE("Error parsing server config: %s", jsonError.c_str());
}

bool ServerConfigImpl::ContainsKey(std::string key){
	return config.object_items().find(key)!=config.object_items().end();
}


ServerConfig::ServerConfig() : p_impl(std::make_shared<ServerConfigImpl>()){
}

ServerConfig::~ServerConfig(){
}

ServerConfig* ServerConfig::GetSharedInstance(){
	if(!sharedInstance)
		sharedInstance=new ServerConfig();
	return sharedInstance;
}

int32_t ServerConfig::GetInt(std::string name, int32_t fallback){
	return p_impl->GetInt(std::move(name), fallback);
}

double ServerConfig::GetDouble(std::string name, double fallback){
	return p_impl->GetDouble(std::move(name), fallback);
}

std::string ServerConfig::GetString(std::string name, std::string fallback){
	return p_impl->GetString(std::move(name), fallback);
}

bool ServerConfig::GetBoolean(std::string name, bool fallback){
	return p_impl->GetBoolean(std::move(name), fallback);
}

void ServerConfig::Update(std::string jsonString){
	p_impl->Update(std::move(jsonString));
}
