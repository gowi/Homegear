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

#include "EventHandler.h"
#include "../GD/GD.h"
#include "../../Modules/Base/BaseLib.h"

EventHandler::EventHandler()
{
}

EventHandler::~EventHandler()
{
	_disposing = true;
	_stopThread = true;
	uint32_t i = 0;
	while(_triggerThreadCount > 0 && i < 300)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		i++;
	}
	if(_mainThread.joinable()) _mainThread.join();
}

void EventHandler::init()
{
	_rpcDecoder = std::unique_ptr<BaseLib::RPC::RPCDecoder>(new BaseLib::RPC::RPCDecoder(GD::bl.get()));
	_rpcEncoder = std::unique_ptr<BaseLib::RPC::RPCEncoder>(new BaseLib::RPC::RPCEncoder(GD::bl.get()));
}

void EventHandler::mainThread()
{
	while(!_stopThread && !_disposing)
	{
		try
		{
			int64_t currentTime = BaseLib::HelperFunctions::getTime();
			_eventsMutex.lock();
			if(!_timedEvents.empty() && _timedEvents.begin()->first <= currentTime)
			{
				std::shared_ptr<Event> event = _timedEvents.begin()->second;
				_eventsMutex.unlock();
				std::shared_ptr<BaseLib::RPC::RPCVariable> result;
				if(event->enabled) result = GD::rpcServers.begin()->second.callMethod(event->eventMethod, event->eventMethodParameters);
				event->lastRaised = currentTime;
				if(result && result->errorStruct)
				{
					GD::out.printError("Could not execute RPC method \"" + event->eventMethod + "\" for timed event \"" + event->name + "\". Error struct:");
					result->print();
				}
				save(event);
				if(event->recurEvery == 0 || (event->endTime > 0 && currentTime >= event->endTime))
				{
					GD::out.printInfo("Info: Removing event " + event->name + ", because the end time is reached.");
					remove(event->name);
				}
				else if(event->recurEvery > 0)
				{
					uint64_t nextExecution = getNextExecution(event->eventTime, event->recurEvery);
					GD::out.printInfo("Info: Next execution for event " + event->name + ": " + std::to_string(nextExecution));
					_eventsMutex.lock();
					//We don't call removeTimedEvents here, so we don't need to release the lock. Otherwise there is the possibility that the event
					//is recreated after being deleted.
					for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _timedEvents.begin(); i != _timedEvents.end(); ++i)
					{
						if(i->second->id == event->id)
						{
							_timedEvents.erase(i);
							break;
						}
					}
					while(_timedEvents.find(nextExecution) != _timedEvents.end()) nextExecution++;
					_timedEvents[nextExecution] = event;
					_eventsMutex.unlock();
				}
				else removeTimedEvent(event->id);
			}
			else if(!_eventsToReset.empty() && _eventsToReset.begin()->first <= currentTime)
			{
				std::shared_ptr<Event> event = _eventsToReset.begin()->second;
				GD::out.printInfo("Info: Resetting event " + event->name + ".");
				_eventsMutex.unlock();
				std::shared_ptr<BaseLib::RPC::RPCVariable> result;
				if(event->enabled) result = GD::rpcServers.begin()->second.callMethod(event->resetMethod, event->resetMethodParameters);
				if(result && result->errorStruct)
				{
					GD::out.printError("Could not execute RPC method \"" + event->eventMethod + "\" for timed event \"" + event->name + "\". Error struct:");
					result->print();
				}
				removeEventToReset(event->id);
				event->lastReset = currentTime;
				save(event);
			}
			else if(!_timesToReset.empty() && _timesToReset.begin()->first <= currentTime)
			{
				std::shared_ptr<Event> event = _timesToReset.begin()->second;
				GD::out.printInfo("Info: Resetting initial time for event " + event->name + ".");
				_eventsMutex.unlock();
				removeTimeToReset(event->id);
				event->lastReset = currentTime;
				event->currentTime = 0;
				save(event);
			}
			else
			{
				_eventsMutex.unlock();
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
			}

			_mainThreadMutex.lock();
			if(_timedEvents.empty() && _eventsToReset.empty() && _timesToReset.empty()) _stopThread = true;
			_mainThreadMutex.unlock();
		}
		catch(const std::exception& ex)
		{
			_eventsMutex.unlock();
			_mainThreadMutex.unlock();
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
		}
		catch(BaseLib::Exception& ex)
		{
			_eventsMutex.unlock();
			_mainThreadMutex.unlock();
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
		}
		catch(...)
		{
			_eventsMutex.unlock();
			_mainThreadMutex.unlock();
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
		}
	}
}

std::shared_ptr<BaseLib::RPC::RPCVariable> EventHandler::add(std::shared_ptr<BaseLib::RPC::RPCVariable> eventDescription)
{
	try
	{
		if(eventDescription->type != BaseLib::RPC::RPCVariableType::rpcStruct) return BaseLib::RPC::RPCVariable::createError(-5, "Parameter is not of type Struct.");
		if(eventDescription->structValue->find("ID") == eventDescription->structValue->end()) return BaseLib::RPC::RPCVariable::createError(-5, "No id specified.");
		if(eventDescription->structValue->find("TYPE") == eventDescription->structValue->end()) return BaseLib::RPC::RPCVariable::createError(-5, "No type specified.");

		std::shared_ptr<Event> event(new Event());
		event->type = (Event::Type::Enum)eventDescription->structValue->at("TYPE")->integerValue;
		event->name = eventDescription->structValue->at("ID")->stringValue;
		if(event->name.size() > 100) return BaseLib::RPC::RPCVariable::createError(-5, "Event ID is too long.");
		if(eventExists(event->name)) return BaseLib::RPC::RPCVariable::createError(-5, "An event with this ID already exists.");

		if(eventDescription->structValue->find("EVENTMETHOD") == eventDescription->structValue->end() || eventDescription->structValue->at("EVENTMETHOD")->stringValue.empty()) return BaseLib::RPC::RPCVariable::createError(-5, "No event method specified.");
		event->eventMethod = eventDescription->structValue->at("EVENTMETHOD")->stringValue;
		if(event->eventMethod.size() > 100) return BaseLib::RPC::RPCVariable::createError(-5, "Event method string is too long.");
		if(eventDescription->structValue->find("EVENTMETHODPARAMS") != eventDescription->structValue->end())
		{
			event->eventMethodParameters = eventDescription->structValue->at("EVENTMETHODPARAMS");
		}

		if(event->type == Event::Type::Enum::triggered)
		{
			if(eventDescription->structValue->find("PEERID") == eventDescription->structValue->end() || eventDescription->structValue->at("PEERID")->integerValue == 0) return BaseLib::RPC::RPCVariable::createError(-5, "No peer id specified.");
			event->peerID = eventDescription->structValue->at("PEERID")->integerValue;
			if(eventDescription->structValue->find("PEERCHANNEL") != eventDescription->structValue->end()) event->peerChannel = eventDescription->structValue->at("PEERCHANNEL")->integerValue;

			if(eventDescription->structValue->find("VARIABLE") == eventDescription->structValue->end() || eventDescription->structValue->at("VARIABLE")->stringValue.empty()) return BaseLib::RPC::RPCVariable::createError(-5, "No variable specified.");
			event->variable = eventDescription->structValue->at("VARIABLE")->stringValue;
			if(event->variable.size() > 100) return BaseLib::RPC::RPCVariable::createError(-5, "Variable string is too long.");
			if(eventDescription->structValue->find("TRIGGER") == eventDescription->structValue->end()) return BaseLib::RPC::RPCVariable::createError(-5, "No trigger specified.");
			event->trigger = (Event::Trigger::Enum)eventDescription->structValue->at("TRIGGER")->integerValue;
			if(eventDescription->structValue->find("TRIGGERVALUE") != eventDescription->structValue->end())
				event->triggerValue = eventDescription->structValue->at("TRIGGERVALUE");
			if((int32_t)event->trigger >= (int32_t)Event::Trigger::value && (!event->triggerValue || event->triggerValue->type == BaseLib::RPC::RPCVariableType::rpcVoid)) return BaseLib::RPC::RPCVariable::createError(-5, "No trigger value specified.");

			if(eventDescription->structValue->find("ENABLED") != eventDescription->structValue->end()) event->enabled = eventDescription->structValue->at("ENABLED")->booleanValue;

			if(eventDescription->structValue->find("RESETAFTER") != eventDescription->structValue->end())
			{
				if(eventDescription->structValue->find("RESETMETHOD") == eventDescription->structValue->end() || eventDescription->structValue->at("RESETMETHOD")->stringValue.empty()) return BaseLib::RPC::RPCVariable::createError(-5, "No reset method specified.");
				event->resetMethod = eventDescription->structValue->at("RESETMETHOD")->stringValue;
				if(event->resetMethod.size() > 100) return BaseLib::RPC::RPCVariable::createError(-5, "Reset method string is too long.");
				if(eventDescription->structValue->find("RESETMETHODPARAMS") != eventDescription->structValue->end())
				{
					event->resetMethodParameters = eventDescription->structValue->at("RESETMETHODPARAMS");
				}

				if(eventDescription->structValue->at("RESETAFTER")->type == BaseLib::RPC::RPCVariableType::rpcInteger)
				{
					event->resetAfter = ((uint64_t)eventDescription->structValue->at("RESETAFTER")->integerValue) * 1000;
				}
				else if(eventDescription->structValue->at("RESETAFTER")->type == BaseLib::RPC::RPCVariableType::rpcStruct)
				{
					std::shared_ptr<BaseLib::RPC::RPCVariable> resetStruct = eventDescription->structValue->at("RESETAFTER");
					if(resetStruct->structValue->find("INITIALTIME") == resetStruct->structValue->end() || resetStruct->structValue->at("INITIALTIME")->integerValue == 0) return BaseLib::RPC::RPCVariable::createError(-5, "Initial time in reset struct not specified.");
					event->initialTime = ((uint64_t)resetStruct->structValue->at("INITIALTIME")->integerValue) * 1000;
					if(resetStruct->structValue->find("OPERATION") != resetStruct->structValue->end())
						event->operation = (Event::Operation::Enum)resetStruct->structValue->at("OPERATION")->integerValue;
					if(resetStruct->structValue->find("FACTOR") != resetStruct->structValue->end())
					{
						event->factor = resetStruct->structValue->at("FACTOR")->floatValue;
						if(event->factor <= 0) return BaseLib::RPC::RPCVariable::createError(-5, "Factor is less or equal 0. Please provide a positive value");
					}
					if(resetStruct->structValue->find("LIMIT") != resetStruct->structValue->end())
					{
						if(resetStruct->structValue->at("LIMIT")->integerValue < 0) return BaseLib::RPC::RPCVariable::createError(-5, "Limit is negative. Please provide a positive value");
						event->limit = ((uint64_t)resetStruct->structValue->at("LIMIT")->integerValue) * 1000;
					}
					if(resetStruct->structValue->find("RESETAFTER") != resetStruct->structValue->end())
						event->resetAfter = ((uint64_t)resetStruct->structValue->at("RESETAFTER")->integerValue) * 1000;
					if(event->resetAfter == 0) return BaseLib::RPC::RPCVariable::createError(-5, "RESETAFTER is not specified or 0.");
				}
			}
			_eventsMutex.lock();
			_triggeredEvents[event->peerID][event->peerChannel][event->variable].push_back(event);
			_eventsMutex.unlock();
		}
		else
		{
			if(eventDescription->structValue->find("EVENTTIME") != eventDescription->structValue->end())
				event->eventTime = ((uint64_t)eventDescription->structValue->at("EVENTTIME")->integerValue) * 1000;
			if(event->eventTime == 0) event->eventTime = BaseLib::HelperFunctions::getTimeSeconds();
			if(eventDescription->structValue->find("ENDTIME") != eventDescription->structValue->end())
				event->endTime = ((uint64_t)eventDescription->structValue->at("ENDTIME")->integerValue) * 1000;
			if(eventDescription->structValue->find("RECUREVERY") != eventDescription->structValue->end())
				event->recurEvery = ((uint64_t)eventDescription->structValue->at("RECUREVERY")->integerValue) * 1000;
			uint64_t nextExecution = getNextExecution(event->eventTime, event->recurEvery);
			_eventsMutex.lock();
			while(_timedEvents.find(nextExecution) != _timedEvents.end()) nextExecution++;
			_timedEvents[nextExecution] = event;
			_eventsMutex.unlock();
			_mainThreadMutex.lock();
			if(_stopThread || !_mainThread.joinable())
			{
				if(_mainThread.joinable()) _mainThread.join();
				_stopThread = false;
				_mainThread = std::thread(&EventHandler::mainThread, this);
			}
			_mainThreadMutex.unlock();
		}
		save(event);
		return std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable(BaseLib::RPC::RPCVariableType::rpcVoid));
	}
	catch(const std::exception& ex)
    {
		_eventsMutex.unlock();
		_mainThreadMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(BaseLib::Exception& ex)
    {
    	_eventsMutex.unlock();
    	_mainThreadMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	_eventsMutex.unlock();
    	_mainThreadMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    return BaseLib::RPC::RPCVariable::createError(-32500, "Unknown application error.");
}

std::shared_ptr<BaseLib::RPC::RPCVariable> EventHandler::list(int32_t type, uint64_t peerID, int32_t peerChannel, std::string variable)
{
	try
	{
		std::vector<std::shared_ptr<Event>> events;
		_eventsMutex.lock();
		//Copy all events first, because listEvents takes very long and we don't want to lock _eventsMutex too long
		if(type < 0 || type == 1)
		{
			for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _timedEvents.begin(); i != _timedEvents.end(); ++i)
			{
				events.push_back(i->second);
			}
		}
		if(type <= 0)
		{
			for(std::map<uint64_t, std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>>::iterator iChannels = _triggeredEvents.begin(); iChannels != _triggeredEvents.end(); ++iChannels)
			{
				for(std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>::iterator iVariables = iChannels->second.begin(); iVariables != iChannels->second.end(); ++iVariables)
				{
					for(std::map<std::string, std::vector<std::shared_ptr<Event>>>::iterator iEvents = iVariables->second.begin(); iEvents != iVariables->second.end(); ++iEvents)
					{
						for(std::vector<std::shared_ptr<Event>>::iterator iEvent = iEvents->second.begin(); iEvent != iEvents->second.end(); ++iEvent)
						{
							if(peerID == 0 && variable.empty()) events.push_back(*iEvent);
							else if(peerID > 0)
							{
								if((*iEvent)->peerID == peerID && (*iEvent)->peerChannel == peerChannel)
								{
									if(variable.empty() || (*iEvent)->variable == variable) events.push_back(*iEvent);
								}
							}
							else if(!variable.empty() && (*iEvent)->variable == variable) events.push_back(*iEvent);
						}
					}
				}
			}
		}
		_eventsMutex.unlock();

		std::shared_ptr<BaseLib::RPC::RPCVariable> eventList(new BaseLib::RPC::RPCVariable(BaseLib::RPC::RPCVariableType::rpcArray));
		for(std::vector<std::shared_ptr<Event>>::iterator i = events.begin(); i != events.end(); ++i)
		{
			//listEvents really needs a lot of resources, so wait a little bit after each event
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
			std::shared_ptr<BaseLib::RPC::RPCVariable> event(new BaseLib::RPC::RPCVariable(BaseLib::RPC::RPCVariableType::rpcStruct));

			if((*i)->type == Event::Type::timed)
			{
				event->structValue->insert(BaseLib::RPC::RPCStructElement("TYPE", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((int32_t)(*i)->type))));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("ID", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->name))));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("ENABLED", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->enabled))));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("EVENTTIME", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)((*i)->eventTime / 1000)))));
				if((*i)->recurEvery > 0)
				{
					event->structValue->insert(BaseLib::RPC::RPCStructElement("RECUREVERY", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)((*i)->recurEvery / 1000)))));
					if((*i)->endTime > 0) event->structValue->insert(BaseLib::RPC::RPCStructElement("ENDTIME", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)((*i)->endTime / 1000)))));
				}
				event->structValue->insert(BaseLib::RPC::RPCStructElement("EVENTMETHOD", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->eventMethod))));
				if((*i)->eventMethodParameters) event->structValue->insert(BaseLib::RPC::RPCStructElement("EVENTMETHODPARAMS", (*i)->eventMethodParameters));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("LASTRAISED", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)((*i)->lastRaised / 1000)))));
			}
			else
			{
				event->structValue->insert(BaseLib::RPC::RPCStructElement("TYPE", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((int32_t)(*i)->type))));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("ID", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->name))));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("ENABLED", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->enabled))));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("PEERID", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)(*i)->peerID))));
				if((*i)->peerChannel > -1) event->structValue->insert(BaseLib::RPC::RPCStructElement("PEERCHANNEL", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->peerChannel))));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("VARIABLE", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->variable))));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("TRIGGER", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((int32_t)(*i)->trigger))));
				if((*i)->triggerValue) event->structValue->insert(BaseLib::RPC::RPCStructElement("TRIGGERVALUE", (*i)->triggerValue));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("EVENTMETHOD", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->eventMethod))));
				if((*i)->eventMethodParameters) event->structValue->insert(BaseLib::RPC::RPCStructElement("EVENTMETHODPARAMS", (*i)->eventMethodParameters));
				if((*i)->resetAfter > 0 || (*i)->initialTime > 0)
				{
					if((*i)->initialTime > 0)
					{
						std::shared_ptr<BaseLib::RPC::RPCVariable> resetStruct(new BaseLib::RPC::RPCVariable(BaseLib::RPC::RPCVariableType::rpcStruct));

						resetStruct->structValue->insert(BaseLib::RPC::RPCStructElement("INITIALTIME", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)((*i)->initialTime / 1000)))));
						resetStruct->structValue->insert(BaseLib::RPC::RPCStructElement("RESETAFTER", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)((*i)->resetAfter / 1000)))));
						resetStruct->structValue->insert(BaseLib::RPC::RPCStructElement("OPERATION", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((int32_t)(*i)->operation))));
						resetStruct->structValue->insert(BaseLib::RPC::RPCStructElement("FACTOR", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->factor))));
						resetStruct->structValue->insert(BaseLib::RPC::RPCStructElement("LIMIT", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)((*i)->limit / 1000)))));
						resetStruct->structValue->insert(BaseLib::RPC::RPCStructElement("CURRENTTIME", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->currentTime == 0 ? (uint32_t)((*i)->initialTime / 1000) : (uint32_t)((*i)->currentTime / 1000)))));

						event->structValue->insert(BaseLib::RPC::RPCStructElement("RESETAFTER", resetStruct));
					}
					else event->structValue->insert(BaseLib::RPC::RPCStructElement("RESETAFTER", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)((*i)->resetAfter / 1000)))));
					event->structValue->insert(BaseLib::RPC::RPCStructElement("RESETMETHOD", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((*i)->resetMethod))));
					if((*i)->eventMethodParameters) event->structValue->insert(BaseLib::RPC::RPCStructElement("RESETMETHODPARAMS", (*i)->resetMethodParameters));
					event->structValue->insert(BaseLib::RPC::RPCStructElement("LASTRESET", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)((*i)->lastReset / 1000)))));
				}
				event->structValue->insert(BaseLib::RPC::RPCStructElement("LASTVALUE", (*i)->lastValue));
				event->structValue->insert(BaseLib::RPC::RPCStructElement("LASTRAISED", std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable((uint32_t)((*i)->lastRaised / 1000)))));
			}

			eventList->arrayValue->push_back(event);
		}
		return eventList;
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
    _eventsMutex.unlock();
    return BaseLib::RPC::RPCVariable::createError(-32500, "Unknown application error.");
}

std::shared_ptr<BaseLib::RPC::RPCVariable> EventHandler::remove(std::string name)
{
	try
	{
		_eventsMutex.lock();
		std::shared_ptr<Event> event;
		for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _timedEvents.begin(); i != _timedEvents.end(); ++i)
		{
			if(i->second->name == name)
			{
				event = i->second;
				_timedEvents.erase(i);
				break;
			}
		}
		if(!event)
		{
			for(std::map<uint64_t, std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>>::iterator peerID = _triggeredEvents.begin(); peerID != _triggeredEvents.end(); ++peerID)
			{
				for(std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>::iterator channel = peerID->second.begin(); channel != peerID->second.end(); ++channel)
				{
					for(std::map<std::string, std::vector<std::shared_ptr<Event>>>::iterator variable = channel->second.begin(); variable != channel->second.end(); ++variable)
					{
						for(std::vector<std::shared_ptr<Event>>::iterator currentEvent = variable->second.begin(); currentEvent != variable->second.end(); ++currentEvent)
						{
							if((*currentEvent)->name == name)
							{
								event = *currentEvent;
								_triggeredEvents[peerID->first][channel->first][variable->first].erase(currentEvent);
								if(_triggeredEvents[peerID->first][channel->first][variable->first].empty()) _triggeredEvents[peerID->first][channel->first].erase(variable->first);
								if(_triggeredEvents[peerID->first][channel->first].empty()) _triggeredEvents[peerID->first].erase(channel->first);
								if(_triggeredEvents[peerID->first].empty()) _triggeredEvents.erase(peerID->first);
								break;
							}
						}
					}
				}
			}
		}
		_eventsMutex.unlock();
		if(!event) return BaseLib::RPC::RPCVariable::createError(-5, "Event not found.");
		if(event && event->type == Event::Type::triggered)
		{
			removeEventToReset(event->id);
			removeTimeToReset(event->id);
		}

		_databaseMutex.lock();
		GD::db.deleteEvent(name);
		_databaseMutex.unlock();
		return std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable(BaseLib::RPC::RPCVariableType::rpcVoid));
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
    _eventsMutex.unlock();
    _databaseMutex.unlock();
    return BaseLib::RPC::RPCVariable::createError(-32500, "Unknown application error.");
}

std::shared_ptr<BaseLib::RPC::RPCVariable> EventHandler::enable(std::string name, bool enabled)
{
	try
	{
		_eventsMutex.lock();
		std::shared_ptr<Event> event;
		for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _timedEvents.begin(); i != _timedEvents.end(); ++i)
		{
			if(i->second->name == name)
			{
				event = i->second;
				break;
			}
		}
		if(!event)
		{
			for(std::map<uint64_t, std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>>::iterator iChannels = _triggeredEvents.begin(); iChannels != _triggeredEvents.end(); ++iChannels)
			{
				for(std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>::iterator iVariables = iChannels->second.begin(); iVariables != iChannels->second.end(); ++iVariables)
				{
					for(std::map<std::string, std::vector<std::shared_ptr<Event>>>::iterator iEvents = iVariables->second.begin(); iEvents != iVariables->second.end(); ++iEvents)
					{
						for(std::vector<std::shared_ptr<Event>>::iterator iEvent = iEvents->second.begin(); iEvent != iEvents->second.end(); ++iEvent)
						{
							if((*iEvent)->name == name)
							{
								event = *iEvent;
								break;
							}
						}
					}
				}
			}
		}
		_eventsMutex.unlock();

		if(!event) return BaseLib::RPC::RPCVariable::createError(-5, "Event not found.");
		if(enabled) event->enabled = true;
		else
		{
			event->enabled = false;
			removeEventToReset(event->id);
		}
		save(event);
		return std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable(BaseLib::RPC::RPCVariableType::rpcVoid));
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
    _eventsMutex.unlock();
    _databaseMutex.unlock();
    return BaseLib::RPC::RPCVariable::createError(-32500, "Unknown application error.");
}

std::shared_ptr<BaseLib::RPC::RPCVariable> EventHandler::abortReset(std::string name)
{
	try
	{
		_eventsMutex.lock();
		std::shared_ptr<Event> event;
		for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _timedEvents.begin(); i != _timedEvents.end(); ++i)
		{
			if(i->second->name == name)
			{
				event = i->second;
				break;
			}
		}
		if(!event)
		{
			for(std::map<uint64_t, std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>>::iterator iChannels = _triggeredEvents.begin(); iChannels != _triggeredEvents.end(); ++iChannels)
			{
				for(std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>::iterator iVariables = iChannels->second.begin(); iVariables != iChannels->second.end(); ++iVariables)
				{
					for(std::map<std::string, std::vector<std::shared_ptr<Event>>>::iterator iEvents = iVariables->second.begin(); iEvents != iVariables->second.end(); ++iEvents)
					{
						for(std::vector<std::shared_ptr<Event>>::iterator iEvent = iEvents->second.begin(); iEvent != iEvents->second.end(); ++iEvent)
						{
							if((*iEvent)->name == name)
							{
								event = *iEvent;
								break;
							}
						}
					}
				}
			}
		}
		_eventsMutex.unlock();

		removeEventToReset(event->id);
		save(event);
		return std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable(BaseLib::RPC::RPCVariableType::rpcVoid));
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
    _eventsMutex.unlock();
    _databaseMutex.unlock();
    return BaseLib::RPC::RPCVariable::createError(-32500, "Unknown application error.");
}

void EventHandler::trigger(uint64_t peerID, int32_t channel, std::shared_ptr<std::vector<std::string>> variables, std::shared_ptr<std::vector<std::shared_ptr<BaseLib::RPC::RPCVariable>>> values)
{
	try
	{
		if(_disposing) return;
		std::thread t(&EventHandler::triggerThreadMultipleVariables, this, peerID, channel, variables, values);
		BaseLib::Threads::setThreadPriority(GD::bl.get(), t.native_handle(), GD::bl->settings.eventTriggerThreadPriority(), GD::bl->settings.eventTriggerThreadPolicy());
		t.detach();
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

uint64_t EventHandler::getNextExecution(uint64_t startTime, uint64_t recurEvery)
{
	try
	{
		uint64_t time = BaseLib::HelperFunctions::getTime();
		if(startTime >= time) return startTime;
		if(recurEvery == 0) return -1;
		uint64_t difference = time - startTime;
		return time + (recurEvery - (difference % recurEvery));
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
    return -1;
}

void EventHandler::trigger(uint64_t peerID, int32_t channel, std::string& variable, std::shared_ptr<BaseLib::RPC::RPCVariable>& value)
{
	try
	{
		if(_disposing) return;
		std::thread t(&EventHandler::triggerThread, this, peerID, channel, variable, value);
		BaseLib::Threads::setThreadPriority(GD::bl.get(), t.native_handle(), GD::bl->settings.eventTriggerThreadPriority(), GD::bl->settings.eventTriggerThreadPolicy());
		t.detach();
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

void EventHandler::triggerThreadMultipleVariables(uint64_t peerID, int32_t channel, std::shared_ptr<std::vector<std::string>> variables, std::shared_ptr<std::vector<std::shared_ptr<BaseLib::RPC::RPCVariable>>> values)
{
	_triggerThreadCount++;
	try
	{
		for(uint32_t i = 0; i < variables->size(); i++)
		{
			triggerThread(peerID, channel, variables->at(i), values->at(i));
		}
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
	_triggerThreadCount--;
}

void EventHandler::removeEventToReset(uint32_t id)
{
	_eventsMutex.lock();
	try
	{
		for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _eventsToReset.begin(); i != _eventsToReset.end(); ++i)
		{
			if(i->second->id == id)
			{
				_eventsToReset.erase(i);
				_eventsMutex.unlock();
				return;
			}
		}
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
    _eventsMutex.unlock();
}

void EventHandler::removeTimeToReset(uint32_t id)
{
	_eventsMutex.lock();
	try
	{
		for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _timesToReset.begin(); i != _timesToReset.end(); ++i)
		{
			if(i->second->id == id)
			{
				_timesToReset.erase(i);
				_eventsMutex.unlock();
				return;
			}
		}
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
    _eventsMutex.unlock();
}

void EventHandler::removeTimedEvent(uint32_t id)
{
	_eventsMutex.lock();
	try
	{
		for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _timedEvents.begin(); i != _timedEvents.end(); ++i)
		{
			if(i->second->id == id)
			{
				_timedEvents.erase(i);
				_eventsMutex.unlock();
				return;
			}
		}
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
    _eventsMutex.unlock();
}

bool EventHandler::eventExists(uint32_t id)
{
	_eventsMutex.lock();
	try
	{
		for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _timedEvents.begin(); i != _timedEvents.end(); ++i)
		{
			if(i->second->id == id)
			{
				_eventsMutex.unlock();
				return true;
			}
		}
		for(std::map<uint64_t, std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>>::iterator peerID = _triggeredEvents.begin(); peerID != _triggeredEvents.end(); ++peerID)
		{
			for(std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>::iterator channel = peerID->second.begin(); channel != peerID->second.end(); ++channel)
			{
				for(std::map<std::string, std::vector<std::shared_ptr<Event>>>::iterator variable = channel->second.begin(); variable != channel->second.end(); ++variable)
				{
					for(std::vector<std::shared_ptr<Event>>::iterator event = variable->second.begin(); event != variable->second.end(); ++event)
					{
						if((*event)->id == id)
						{
							_eventsMutex.unlock();
							return true;
						}
					}
				}
			}
		}
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
    _eventsMutex.unlock();
    return false;
}

bool EventHandler::eventExists(std::string name)
{
	_eventsMutex.lock();
	try
	{
		for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _timedEvents.begin(); i != _timedEvents.end(); ++i)
		{
			if(i->second->name == name)
			{
				_eventsMutex.unlock();
				return true;
			}
		}
		for(std::map<uint64_t, std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>>::iterator peerID = _triggeredEvents.begin(); peerID != _triggeredEvents.end(); ++peerID)
		{
			for(std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>::iterator channel = peerID->second.begin(); channel != peerID->second.end(); ++channel)
			{
				for(std::map<std::string, std::vector<std::shared_ptr<Event>>>::iterator variable = channel->second.begin(); variable != channel->second.end(); ++variable)
				{
					for(std::vector<std::shared_ptr<Event>>::iterator event = variable->second.begin(); event != variable->second.end(); ++event)
					{
						if((*event)->name == name)
						{
							_eventsMutex.unlock();
							return true;
						}
					}
				}
			}
		}
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
    _eventsMutex.unlock();
    return false;
}

std::shared_ptr<BaseLib::RPC::RPCVariable> EventHandler::trigger(std::string name)
{
	try
	{
		_eventsMutex.lock();
		std::shared_ptr<Event> event;
		for(std::map<uint64_t, std::shared_ptr<Event>>::iterator i = _timedEvents.begin(); i != _timedEvents.end(); ++i)
		{
			if(i->second->name == name)
			{
				event = i->second;
				break;
			}
		}
		if(!event)
		{
			for(std::map<uint64_t, std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>>::iterator peerID = _triggeredEvents.begin(); peerID != _triggeredEvents.end(); ++peerID)
			{
				for(std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>::iterator channel = peerID->second.begin(); channel != peerID->second.end(); ++channel)
				{
					for(std::map<std::string, std::vector<std::shared_ptr<Event>>>::iterator variable = channel->second.begin(); variable != channel->second.end(); ++variable)
					{
						for(std::vector<std::shared_ptr<Event>>::iterator currentEvent = variable->second.begin(); currentEvent != variable->second.end(); ++currentEvent)
						{
							if((*currentEvent)->name == name)
							{
								event = *currentEvent;
								break;
							}
						}
					}
				}
			}
		}
		_eventsMutex.unlock();

		if(!event) return BaseLib::RPC::RPCVariable::createError(-5, "Event not found.");
		uint64_t currentTime = BaseLib::HelperFunctions::getTime();
		event->lastRaised = currentTime;
		GD::out.printInfo("Info: Event \"" + event->name + "\" raised for peer with id " + std::to_string(event->peerID) + ", channel " + std::to_string(event->peerChannel) + " and variable \"" + event->variable + "\". Trigger: \"manual\"");
		std::shared_ptr<BaseLib::RPC::RPCVariable> result = GD::rpcServers.begin()->second.callMethod(event->eventMethod, event->eventMethodParameters);

		postTriggerTasks(event, result, currentTime);

		return std::shared_ptr<BaseLib::RPC::RPCVariable>(new BaseLib::RPC::RPCVariable(BaseLib::RPC::RPCVariableType::rpcVoid));
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
    _eventsMutex.unlock();
    _databaseMutex.unlock();
    return BaseLib::RPC::RPCVariable::createError(-32500, "Unknown application error.");
}

void EventHandler::triggerThread(uint64_t peerID, int32_t channel, std::string variable, std::shared_ptr<BaseLib::RPC::RPCVariable> value)
{
	_triggerThreadCount++;
	try
	{
		_eventsMutex.lock();
		if(!value || _triggeredEvents.find(peerID) == _triggeredEvents.end())
		{
			_triggerThreadCount--;
			_eventsMutex.unlock();
			return;
		}
		std::map<int32_t, std::map<std::string, std::vector<std::shared_ptr<Event>>>>* channels = &_triggeredEvents.at(peerID);
		if(channels->find(channel) == channels->end())
		{
			_triggerThreadCount--;
			_eventsMutex.unlock();
			return;
		}
		std::map<std::string, std::vector<std::shared_ptr<Event>>>* variables = &channels->at(channel);
		if(variables->find(variable) == variables->end())
		{
			_triggerThreadCount--;
			_eventsMutex.unlock();
			return;
		}
		std::vector<std::shared_ptr<Event>>* events = &variables->at(variable);
		std::vector<std::shared_ptr<Event>> triggeredEvents;
		uint64_t currentTime = BaseLib::HelperFunctions::getTime();
		for(std::vector<std::shared_ptr<Event>>::iterator i = events->begin(); i !=  events->end(); ++i)
		{
			//Don't raise the same event multiple times
			if(!(*i)->enabled || ((*i)->lastValue && *((*i)->lastValue) == *value && currentTime - (*i)->lastRaised < 5000)) continue;
			triggeredEvents.push_back(*i);
		}
		_eventsMutex.unlock();
		for(std::vector<std::shared_ptr<Event>>::iterator i = triggeredEvents.begin(); i !=  triggeredEvents.end(); ++i)
		{
			std::shared_ptr<BaseLib::RPC::RPCVariable> result;
			(*i)->lastValue = value;
			if(((int32_t)(*i)->trigger) < 8)
			{
				//Comparison with previous value
				if((*i)->trigger == Event::Trigger::updated)
				{
					GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"updated\"");
					(*i)->lastRaised = currentTime;
					result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
				}
				else if((*i)->trigger == Event::Trigger::unchanged)
				{
					if(*((*i)->lastValue) == *value)
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"unchanged\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
				else if((*i)->trigger == Event::Trigger::changed)
				{
					if(*((*i)->lastValue) != *value)
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"changed\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
				else if((*i)->trigger == Event::Trigger::greater)
				{
					if(*((*i)->lastValue) > *value)
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"greater\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
				else if((*i)->trigger == Event::Trigger::less)
				{
					if(*((*i)->lastValue) < *value)
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"less\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
				else if((*i)->trigger == Event::Trigger::greaterOrUnchanged)
				{
					if(*((*i)->lastValue) >= *value)
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"greaterOrUnchanged\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
				else if((*i)->trigger == Event::Trigger::lessOrUnchanged)
				{
					if(*((*i)->lastValue) <= *value)
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"lessOrUnchanged\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
			}
			else if((*i)->triggerValue)
			{
				//Comparison with trigger value
				if((*i)->trigger == Event::Trigger::value)
				{
					if(*value == *((*i)->triggerValue))
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"value\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
				else if((*i)->trigger == Event::Trigger::notValue)
				{
					if(*value != *((*i)->triggerValue))
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"notValue\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
				else if((*i)->trigger == Event::Trigger::greaterThanValue)
				{
					if(*value > *((*i)->triggerValue))
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"greaterThanValue\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
				else if((*i)->trigger == Event::Trigger::lessThanValue)
				{
					if(*value < *((*i)->triggerValue))
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"lessThanValue\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
				else if((*i)->trigger == Event::Trigger::greaterOrEqualValue)
				{
					if(*value >= *((*i)->triggerValue))
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"greaterOrEqualValue\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
				else if((*i)->trigger == Event::Trigger::lessOrEqualValue)
				{
					if(*value <= *((*i)->triggerValue))
					{
						GD::out.printInfo("Info: Event \"" + (*i)->name + "\" raised for peer with id " + std::to_string(peerID) + ", channel " + std::to_string(channel) + " and variable \"" + variable + "\". Trigger: \"lessOrEqualValue\"");
						(*i)->lastRaised = currentTime;
						result = GD::rpcServers.begin()->second.callMethod((*i)->eventMethod, (*i)->eventMethodParameters);
					}
				}
			}
			postTriggerTasks(*i, result, currentTime);
		}
	}
	catch(const std::exception& ex)
    {
		_eventsMutex.unlock();
		_mainThreadMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(BaseLib::Exception& ex)
    {
    	_eventsMutex.unlock();
    	_mainThreadMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	_eventsMutex.unlock();
    	_mainThreadMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    _triggerThreadCount--;
}

void EventHandler::postTriggerTasks(std::shared_ptr<Event>& event, std::shared_ptr<BaseLib::RPC::RPCVariable>& rpcResult, uint64_t currentTime)
{
	try
	{
		if(!event)
		{
			GD::out.printError("Error: Could not execute post trigger tasks. event was nullptr.");
			return;
		}
		if(rpcResult && rpcResult->errorStruct)
		{
			GD::out.printError("Error: Could not execute RPC method for event from peer with id " + std::to_string(event->peerID) + ", channel " + std::to_string(event->peerChannel) + " and variable " + event->variable + ". Error struct:");
			rpcResult->print();
		}
		if(event->resetAfter > 0 || event->initialTime > 0)
		{
			try
			{
				removeEventToReset(event->id);
				uint64_t resetTime = currentTime + event->resetAfter;
				if(event->initialTime == 0) //Simple reset
				{
					GD::out.printInfo("Info: Event \"" + event->name + "\" for peer with id " + std::to_string(event->peerID) + ", channel " + std::to_string(event->peerChannel) + " and variable \"" + event->variable + "\" will be reset in " + std::to_string(event->resetAfter / 1000) + " seconds.");

					_eventsMutex.lock();
					while(_eventsToReset.find(resetTime) != _eventsToReset.end()) resetTime++;
					_eventsToReset[resetTime] =  event;
					_eventsMutex.unlock();
				}
				else //Complex reset
				{
					removeTimeToReset(event->id);
					GD::out.printInfo("Info: INITIALTIME for event \"" + event->name + "\" will be reset in " + std::to_string(event->resetAfter / 1000)+ " seconds.");
					_eventsMutex.lock();
					while(_timesToReset.find(resetTime) != _timesToReset.end()) resetTime++;
					_timesToReset[resetTime] = event;
					_eventsMutex.unlock();
					if(event->currentTime == 0) event->currentTime = event->initialTime;
					if(event->factor <= 0)
					{
						GD::out.printWarning("Warning: Factor is less or equal 0. Setting factor to 1. Event from peer with id " + std::to_string(event->peerID) + ", channel " + std::to_string(event->peerChannel) + " and variable " + event->variable + ".");
						event->factor = 1;
					}
					resetTime = currentTime + event->currentTime;
					_eventsMutex.lock();
					while(_eventsToReset.find(resetTime) != _eventsToReset.end()) resetTime++;
					_eventsToReset[resetTime] =  event;
					_eventsMutex.unlock();
					GD::out.printInfo("Info: Event \"" + event->name + "\" will be reset in " + std::to_string(event->currentTime / 1000) + " seconds.");
					if(event->operation == Event::Operation::Enum::addition)
					{
						event->currentTime += event->factor;
						if(event->currentTime > event->limit) event->currentTime = event->limit;
					}
					else if(event->operation == Event::Operation::Enum::subtraction)
					{
						event->currentTime -= event->factor;
						if(event->currentTime < event->limit) event->currentTime = event->limit;
					}
					else if(event->operation == Event::Operation::Enum::multiplication)
					{
						event->currentTime *= event->factor;
						if(event->currentTime > event->limit) event->currentTime = event->limit;
					}
					else if(event->operation == Event::Operation::Enum::division)
					{
						event->currentTime /= event->factor;
						if(event->currentTime < event->limit) event->currentTime = event->limit;
					}
				}
			}
			catch(const std::exception& ex)
			{
				_eventsMutex.unlock();
				GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
			}
			catch(BaseLib::Exception& ex)
			{
				_eventsMutex.unlock();
				GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
			}
			catch(...)
			{
				_eventsMutex.unlock();
				GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
			}

			try
			{
				_mainThreadMutex.lock();
				if(_stopThread || !_mainThread.joinable())
				{
					if(_mainThread.joinable()) _mainThread.join();
					_stopThread = false;
					_mainThread = std::thread(&EventHandler::mainThread, this);
				}
				_mainThreadMutex.unlock();
			}
			catch(const std::exception& ex)
			{
				_mainThreadMutex.unlock();
				GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
			}
			catch(BaseLib::Exception& ex)
			{
				_mainThreadMutex.unlock();
				GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
			}
			catch(...)
			{
				_mainThreadMutex.unlock();
				GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
			}
		}
		save(event);
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

void EventHandler::load()
{
	try
	{
		if(!GD::bl || !_rpcDecoder)
		{
			GD::out.printError("Error: Tried to load events, but event handler is not initialized.");
			return;
		}

		_databaseMutex.lock();
		std::shared_ptr<BaseLib::Database::DataTable> rows = GD::db.getEvents();
		_databaseMutex.unlock();
		for(BaseLib::Database::DataTable::iterator row = rows->begin(); row != rows->end(); ++row)
		{
			std::shared_ptr<Event> event(new Event());
			event->id = row->second.at(0)->intValue;
			event->name = row->second.at(1)->textValue;
			event->type = (Event::Type::Enum)row->second.at(2)->intValue;
			event->peerID = row->second.at(3)->intValue;
			event->peerChannel = row->second.at(4)->intValue;
			event->variable = row->second.at(5)->textValue;
			event->trigger = (Event::Trigger::Enum)row->second.at(6)->intValue;
			event->triggerValue = _rpcDecoder->decodeResponse(row->second.at(7)->binaryValue);
			event->eventMethod = row->second.at(8)->textValue;
			event->eventMethodParameters = _rpcDecoder->decodeResponse(row->second.at(9)->binaryValue);
			event->resetAfter = row->second.at(10)->intValue;
			event->initialTime = row->second.at(11)->intValue;
			event->operation = (Event::Operation::Enum)row->second.at(12)->intValue;
			event->factor = row->second.at(13)->floatValue;
			event->limit = row->second.at(14)->intValue;
			event->resetMethod = row->second.at(15)->textValue;
			event->resetMethodParameters = _rpcDecoder->decodeResponse(row->second.at(16)->binaryValue);
			event->eventTime = row->second.at(17)->intValue;
			event->endTime = row->second.at(18)->intValue;
			event->recurEvery = row->second.at(19)->intValue;
			event->lastValue = _rpcDecoder->decodeResponse(row->second.at(20)->binaryValue);
			event->lastRaised = row->second.at(21)->intValue;
			event->lastReset = row->second.at(22)->intValue;
			event->currentTime = row->second.at(23)->intValue;
			event->enabled = row->second.at(24)->intValue;
			_eventsMutex.lock();
			if(event->eventTime > 0)
			{
				_timedEvents[getNextExecution(event->eventTime, event->recurEvery)] = event;
			}
			else
			{
				_triggeredEvents[event->peerID][event->peerChannel][event->variable].push_back(event);
				if(event->resetAfter > 0 && event->lastReset <= event->lastRaised)
				{
					if(event->initialTime > 0)
					{
						_eventsToReset[event->lastRaised + event->currentTime] = event;
						_timesToReset[event->lastRaised + event->resetAfter] = event;
					}
					else _eventsToReset[event->lastRaised + event->resetAfter] = event;
				}
				else if(event->initialTime > 0) event->currentTime = 0;
			}
			_eventsMutex.unlock();
		}
		_mainThreadMutex.lock();
		if(!_timedEvents.empty() || !_eventsToReset.empty())
		{
			_stopThread = false;
			_mainThread = std::thread(&EventHandler::mainThread, this);
		}
		_mainThreadMutex.unlock();
	}
	catch(const std::exception& ex)
    {
		_eventsMutex.unlock();
		_mainThreadMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(BaseLib::Exception& ex)
    {
    	_eventsMutex.unlock();
    	_mainThreadMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	_eventsMutex.unlock();
    	_mainThreadMutex.unlock();
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void EventHandler::save(std::shared_ptr<Event> event)
{
	std::string eventID = std::to_string(event->id);
	try
	{
		//The eventExists is necessary so we don't safe an event that is being deleted
		if(!event || _disposing) return;
		_databaseMutex.lock();
		if(event->id > 0 && !eventExists(event->id))
		{
			_databaseMutex.unlock();
			return;
		}
		BaseLib::Database::DataRow data;
		if(event->id > 0) data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->id)));
		else data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn()));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->name)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn((int32_t)event->type)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->peerID)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->peerChannel)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->variable)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn((int32_t)event->trigger)));
		std::shared_ptr<std::vector<char>> value = _rpcEncoder->encodeResponse(event->triggerValue);
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(value)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->eventMethod)));
		value = _rpcEncoder->encodeResponse(event->eventMethodParameters);
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(value)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->resetAfter)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->initialTime)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn((int32_t)event->operation)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->factor)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->limit)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->resetMethod)));
		value = _rpcEncoder->encodeResponse(event->resetMethodParameters);
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(value)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->eventTime)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->endTime)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->recurEvery)));
		value = _rpcEncoder->encodeResponse(event->lastValue);
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(value)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->lastRaised)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->lastReset)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->currentTime)));
		data.push_back(std::shared_ptr<BaseLib::Database::DataColumn>(new BaseLib::Database::DataColumn(event->enabled)));
		uint64_t result = GD::db.saveEvent(data);
		if(event->id == 0) event->id = result;
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
    _databaseMutex.unlock();
}
