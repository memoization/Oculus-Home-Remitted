#pragma once
#include <fstream>
#include <string>

class HomeLogger
{
	public:
		void open(const std::string& path);
		void flush();
		void close();
		std::wofstream& write();

	private:
		std::wofstream stream_;
};
extern HomeLogger homeLogger;