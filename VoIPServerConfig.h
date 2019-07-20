//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef TGVOIP_VOIPSERVERCONFIG_H
#define TGVOIP_VOIPSERVERCONFIG_H

#include <memory>
#include <string>
#include <stdint.h>

namespace tgvoip{

class ServerConfigImpl;

class ServerConfig{
public:
	ServerConfig();
	~ServerConfig();
	static ServerConfig* GetSharedInstance();
	int32_t GetInt(std::string name, int32_t fallback);
	double GetDouble(std::string name, double fallback);
	std::string GetString(std::string name, std::string fallback);
	bool GetBoolean(std::string name, bool fallback);
	void Update(std::string jsonString);

private:
	std::shared_ptr<ServerConfigImpl> p_impl;
};
}

#endif //TGVOIP_VOIPSERVERCONFIG_H
