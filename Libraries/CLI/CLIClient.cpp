/* Copyright 2013-2014 Sathya Laufer
 *
 * Homegear is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Homegear is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Homegear.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */

#include "CLIClient.h"
#include "../GD/GD.h"
#include "../../Modules/Base/BaseLib.h"

namespace CLI {

Client::Client()
{
	_fileDescriptor = std::shared_ptr<BaseLib::FileDescriptor>(new BaseLib::FileDescriptor);
}

Client::~Client()
{
	try
	{
		_stopPingThread = true;
		if(_pingThread.joinable()) _pingThread.join();
	}
    catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(BaseLib::Exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void Client::ping()
{
	try
	{
		char buffer[1] = {0};
		while(!_stopPingThread)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2000));
			_sendMutex.lock();
			if(_closed) return;
			if(send(_fileDescriptor->descriptor, buffer, 1, MSG_NOSIGNAL) == -1)
			{
				_closed = true;
				std::cout << std::endl << "Connection closed." << std::endl;
				_sendMutex.unlock();
				return;
			}
			_sendMutex.unlock();
		}
	}
    catch(const std::exception& ex)
    {
    	_sendMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(BaseLib::Exception& ex)
    {
    	_sendMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	_sendMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void Client::start()
{
	try
	{
		for(int32_t i = 0; i < 2; i++)
		{
			_fileDescriptor = GD::bl->fileDescriptorManager.add(socket(AF_UNIX, SOCK_STREAM, 0));
			if(!_fileDescriptor || _fileDescriptor->descriptor == -1)
			{
				GD::out.printError("Could not create socket.");
				return;
			}

			if(GD::bl->debugLevel >= 4 && i == 0) std::cout << "Info: Trying to connect..." << std::endl;
			sockaddr_un remoteAddress;
			remoteAddress.sun_family = AF_UNIX;
			if(GD::socketPath.length() > 107)
			{
				//Check for buffer overflow
				GD::out.printCritical("Critical: Socket path is too long.");
				return;
			}
			strncpy(remoteAddress.sun_path, GD::socketPath.c_str(), 108);
			if(connect(_fileDescriptor->descriptor, (struct sockaddr*)&remoteAddress, strlen(remoteAddress.sun_path) + sizeof(remoteAddress.sun_family)) == -1)
			{
				GD::bl->fileDescriptorManager.shutdown(_fileDescriptor);
				if(i == 0)
				{
					GD::out.printDebug("Debug: Socket closed. Trying again...");
					//When socket was not properly closed, we sometimes need to reconnect
					std::this_thread::sleep_for(std::chrono::milliseconds(2000));
					continue;
				}
				else
				{
					GD::out.printError("Could not connect to socket. Error: " + std::string(strerror(errno)));
					return;
				}
			}
			else break;
		}
		if(GD::bl->debugLevel >= 4) std::cout << "Info: Connected." << std::endl;

		_pingThread = std::thread(&CLI::Client::ping, this);

		rl_bind_key('\t', rl_abort); //no autocompletion

		std::string lastCommand;
		std::string currentCommand;
		char* sendBuffer;
		char receiveBuffer[1025];
		uint32_t bytes;
		while((sendBuffer = readline("> ")) != NULL)
		{
			if(_closed) break;
			bytes = strlen(sendBuffer);
			if(sendBuffer[0] == '\n' || sendBuffer[0] == 0) continue;
			if(strncmp(sendBuffer, "quit", 4) == 0 || strncmp(sendBuffer, "exit", 4) == 0 || strncmp(sendBuffer, "moin", 4) == 0)
			{
				_closed = true;
				//If we close the socket, the socket file gets deleted. We don't want that
				//GD::bl->fileDescriptorManager.close(_fileDescriptor);
				free(sendBuffer);
				return;
			}
			_sendMutex.lock();
			if(_closed) break;
			if(send(_fileDescriptor->descriptor, sendBuffer, bytes, MSG_NOSIGNAL) == -1)
			{
				_sendMutex.unlock();
				GD::out.printError("Error sending to socket.");
				//If we close the socket, the socket file gets deleted. We don't want that
				//GD::bl->fileDescriptorManager.close(_fileDescriptor);
				free(sendBuffer);
				return;
			}
			currentCommand = std::string(sendBuffer);
			if(currentCommand != lastCommand)
			{
				lastCommand = currentCommand;
				add_history(sendBuffer); //Sets sendBuffer to 0
			}
			else
			{
				free(sendBuffer);
				sendBuffer = 0;
			}

			while(true)
			{
				if((bytes = recv(_fileDescriptor->descriptor, receiveBuffer, 1024, 0)) > 0)
				{
					receiveBuffer[bytes] = 0;
					std::cout << receiveBuffer;
					if(bytes < 1024 || (bytes == 1024 && receiveBuffer[bytes - 1] == 0)) break;
				}
				else
				{
					_sendMutex.unlock();
					if(bytes < 0) std::cerr << "Error receiving data from socket." << std::endl;
					else std::cout << "Connection closed." << std::endl;
					//If we close the socket, the socket file gets deleted. We don't want that
					//GD::bl->fileDescriptorManager.close(_fileDescriptor);
					return;
				}
			}
			_sendMutex.unlock();
		}
	}
    catch(const std::exception& ex)
    {
    	GD::out.printError("Couldn't create socket file " + GD::socketPath + ": " + ex.what());;
    }
    catch(BaseLib::Exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

} /* namespace CLI */
