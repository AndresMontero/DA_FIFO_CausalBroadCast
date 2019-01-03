#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <thread>
#include <vector>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <iostream>
#include <numeric>
#include <fstream>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/utility.hpp>
#include <sstream>
#include <unordered_set>
#include <map>
#include <utility>
#include <algorithm>

// VERBOSE flag for more detailed logging
#define VERBOSE 0

using namespace std;
using namespace boost::archive;

// Declare structs and classes used
struct processIdentity
{
	std::string ip_address;
	int port;
};

class messagePacket
{
  public:
	messagePacket() = default;
	messagePacket(int processId, int messageIdStart, int messageIdEnd, int type, std::vector<pair<int, int>> past) : _processId{processId}, _messageIdStart{messageIdStart}, _messageIdEnd{messageIdEnd}, _type{type}, _past{std::move(past)} {}
	int processId() const { return _processId; }
	int messageIdStart() const { return _messageIdStart; }
	int messageIdEnd() const { return _messageIdEnd; }
	int type() const { return _type; }
	const std::vector<pair<int, int>> &past() const { return _past; }

  private:
	friend class boost::serialization::access;

	template <class Archive>
	friend void serialize(Archive &ar, messagePacket &messageP, const unsigned int version);

	int _processId;
	int _messageIdStart;
	int _messageIdEnd;
	int _type;
	std::vector<pair<int, int>> _past;
};

template <typename Archive>
void serialize(Archive &ar, messagePacket &messageP, const unsigned int version)
{

	ar &messageP._processId;
	ar &messageP._messageIdStart;
	ar &messageP._messageIdEnd;
	ar &messageP._type;
	ar &messageP._past;
	if (VERBOSE)
	{
		printf("-------- serialize method -------\n");
		printf("Process ID is %d.\n", messageP._processId);
		printf("Type ID is %d.\n", messageP._type);
		printf("Message ID Start is %d.\n", messageP._messageIdStart);
		printf("Message ID End is %d.\n", messageP._messageIdEnd);
		printf("-------- end of serialize method -------\n");
	}
}

class ComparisonMessagePackets
{
  public:
	bool operator()(messagePacket &a, messagePacket &b)
	{
		return a.messageIdStart() > b.messageIdStart();
	}
};

// Declare global variables
bool stopAll = false;
static int wait_for_start = 1;
static int MAX_MUTEX = 10000;
char message[10000];
std::vector<processIdentity> processIdentities;
int numberOfProcess;
int processId;
int numberOfMessages;
int batchSize = 10;

long processSocket;
struct sockaddr_in *sendAddresses;
thread broadcastThread;
thread *sendThreads;
int *sleepTimers;
const unsigned messageBuffer_max_size = 100000;

vector<int> *peersAgreement;
vector<int> delivered;
vector<priority_queue<messagePacket, std::vector<messagePacket>, ComparisonMessagePackets>> messageBuffer;
std::map<pair<int, int>, string> *messagesToBeSent;
std::vector<pair<int, int>> localPast;
mutex **mutexMessages;
mutex *mutexMsgsToBeSent;
mutex mutexPast;
vector<set<int>> dependency;
vector<set<int>> bufferDuplicateChecker;
vector<std::tuple<char, int, int, int>> logs;
vector<string> dependencies_lines;

// Serialize a message from messagePacket to a string
std::string serializeMessage(messagePacket packet)
{
	// Serialize to string
	std::ostringstream oss;
	text_oarchive oa(oss);
	oa << packet;
	return oss.str();
}

// Deserialize a message from a string to a messagePacket
messagePacket deserializeMessage(char *packet)
{
	// Serialize to string
	std::istringstream iss(packet);
	text_iarchive ia(iss);
	messagePacket auxPacket;
	ia >> auxPacket;
	return auxPacket;
}

// Flush the logs to the respective *.out files
void flush_logs()
{
	int acc = 0;
	for (auto i : delivered)
		acc += i;
	if (VERBOSE)
		printf("Process id = %d has delivered %d batches\n", processId, acc);

	ofstream file;
	file.open("da_proc_" + to_string(processId) + ".out");
	if (file.is_open())
	{
		// flushing the  logs
		for (std::tuple<char, int, int, int> s : logs)
		{
			for (int i = get<2>(s); i <= get<3>(s); i++)
			{
				if (get<0>(s) == 'b')
					file << get<0>(s) << " " << i << "\n";
				else if (get<0>(s) == 'd')
					file << get<0>(s) << " " << get<1>(s) << " " << i << "\n";
			}
		}

		file.close();
	}
	logs.clear();
	if (VERBOSE)
	{
		for (auto b : messageBuffer)
			cout << "size of the buffer for process id " << processId << " " << b.size() << endl;
	}
}

// Handle start signal
static void start(int signum)
{
	wait_for_start = 0;
}

// Handle stop signal
static void stop(int signum)
{
	// Immediately stop network packet processing
	if (VERBOSE)
		printf("Immediately stopping network packet processing.\n");

	// This will immediately stop all the thread from running, and making them joinable without further wait
	// to avoid any memory leakage.
	stopAll = true;

	// Flush the logs
	flush_logs();

	// Reset signal handlers to default
	signal(SIGTERM, SIG_DFL);
	signal(SIGINT, SIG_DFL);

	// Write/flush output file if necessary
	if (VERBOSE)
		printf("Writing output.\n");

	// Joining threads to close them
	if (broadcastThread.joinable())
		broadcastThread.join();
	for (int i = 0; i < numberOfProcess; i++)
	{
		if (sendThreads[i].joinable())
			sendThreads[i].join();
	}
	if (VERBOSE)
		printf("End of the execution of the program\n");

	//exit directly from signal handler
	exit(0);
}

// Initialize the global variables with the necessary starting values
void initializeVariables()
{
	if (VERBOSE)
		printf("Beginning initializeVariables function\n");
	// Initialize all variables and vectors with a size equal to the number of processes
	sendThreads = new thread[numberOfProcess];
	sleepTimers = new int[numberOfProcess];
	delivered = vector<int>(numberOfProcess, 0);
	messageBuffer = vector<priority_queue<messagePacket, std::vector<messagePacket>, ComparisonMessagePackets>>(numberOfProcess, priority_queue<messagePacket, std::vector<messagePacket>, ComparisonMessagePackets>());
	peersAgreement = new vector<int>[numberOfProcess];
	dependency = vector<set<int>>(numberOfProcess, set<int>());
	bufferDuplicateChecker = vector<set<int>>(numberOfProcess, set<int>());
	messagesToBeSent = new map<pair<int, int>, string>[numberOfProcess];
	mutexMessages = new mutex *[numberOfProcess];
	for (int i = 0; i < numberOfProcess; i++)
	{
		bufferDuplicateChecker[i].insert(-1);
		peersAgreement[i].resize(1024 * 1024);
		mutexMessages[i] = new mutex[MAX_MUTEX];
	}
	mutexMsgsToBeSent = new mutex[numberOfProcess];
	if (VERBOSE)
		printf("Ending initializeVariables function\n");
}

// Sends a message through the UDP socket to the specified destination Id.
void sendMessage(int destinationId, std::string message)
{
	if (VERBOSE)
	{
		printf("-------- sendMessage function -------\n");
		printf("Sending message to %d\n", destinationId);
		printf("-------- Content of message -------\n");
		char temp_msg[message.size()];
		strcpy(temp_msg, message.c_str());
		messagePacket temp = deserializeMessage(temp_msg);
		printf("Process ID is %d.\n", temp.processId());
		printf("Type ID is %d.\n", temp.type());
		printf("Message ID Start is %d.\n", temp.messageIdStart());
		printf("Message ID End is %d.\n", temp.messageIdEnd());
		printf("-------- end of sendMessage function -------\n");
	}
	// Send a message through the UDP socket
	int bytes = sendto(processSocket, message.c_str(), message.size(), 0, (struct sockaddr *)&sendAddresses[destinationId], sizeof(sendAddresses[destinationId]));
	if (static_cast<uint>(bytes) != message.size())
	{
		printf("Error when trying to send a message\n");
	}
}

// Keeps sending the messages in the messagesToBeSent map until they receive acknowledgements
void messageSender(int peerNum)
{
	// This infinite loop sends every 1000 nanoseconds all the messages inside the messagesToBeSent for this process,
	// messagesToBeSent contains all the messages that need to be sent and have not received an acknowledgement yet
	while (!stopAll)
	{
		struct timespec sleep_time;
		sleep_time.tv_sec = 0;
		sleep_time.tv_nsec = 1000;
		nanosleep(&sleep_time, NULL);
		mutexMsgsToBeSent[peerNum].lock();
		if (VERBOSE)
		{
			printf("-------- Beg - messageSender function -------\n");
			printf("messageSender has a buffer of message to be sent to %d = %d messages (batches)\n", peerNum, (int)messagesToBeSent[peerNum].size());
			printf("-------- End - messageSender function -------\n");
		}
		for (auto iter : messagesToBeSent[peerNum])
		{
			sendMessage(peerNum, iter.second);
		}
		mutexMsgsToBeSent[peerNum].unlock();
	}
}

// When we receive a message, updates the peersAgreement value for the message, decides if we send and acknowledgement back, and decides if we can deliver it.
void handleMessage(messagePacket receivedMsg, int sender)
{
	// Lock mutex
	mutexMessages[receivedMsg.processId()][(receivedMsg.messageIdStart() / batchSize) % MAX_MUTEX].lock();

	// Increase peersAgreement size dinamically if we received a lot of messages.
	if (int(peersAgreement[receivedMsg.processId()].size()) <= receivedMsg.messageIdStart() / batchSize)
	{
		if (int(peersAgreement[receivedMsg.processId()].size()) <= receivedMsg.messageIdStart() / batchSize)
		{
			peersAgreement[receivedMsg.processId()].resize(receivedMsg.messageIdStart() / batchSize + 1000);
		}
	}

	// If it is the first time you are seeing this message
	if (peersAgreement[receivedMsg.processId()][receivedMsg.messageIdStart() / batchSize] == 0)
	{
		// First time and it is from the same process
		if (receivedMsg.processId() == processId - 1)
		{
			// Increase peersAgreement by 1
			peersAgreement[receivedMsg.processId()][receivedMsg.messageIdStart() / batchSize]++;
			for (int i = 0; i < numberOfProcess; i++)
			{
				// Broadcast to all other processes
				if (i != processId - 1)
				{
					mutexMsgsToBeSent[i].lock();
					messagesToBeSent[i][make_pair(receivedMsg.processId(), receivedMsg.messageIdStart() / batchSize)] = serializeMessage(receivedMsg);
					mutexMsgsToBeSent[i].unlock();
				}
			}
		}
		else
		{
			// First time and it is from another process
			// Set peersAgreement to 2 (we are sure the sender and you have seen it already)
			peersAgreement[receivedMsg.processId()][receivedMsg.messageIdStart() / batchSize] = 2;
			for (int i = 0; i < numberOfProcess; i++)
			{
				if (i != sender && i != processId - 1)
				{
					// Broadcast to all other processes (except the one that sent you this)
					mutexMsgsToBeSent[i].lock();
					messagesToBeSent[i][make_pair(receivedMsg.processId(), receivedMsg.messageIdStart() / batchSize)] = serializeMessage(receivedMsg);
					mutexMsgsToBeSent[i].unlock();
				}
			}
		}
	}
	else
	{
		// In case you have already seen this message
		mutexMsgsToBeSent[sender].lock();
		// If it is a valid acknowledgement remove it from the messagesToBeSent
		if ((receivedMsg.type() == 1) && (messagesToBeSent[sender].find(make_pair(receivedMsg.processId(), receivedMsg.messageIdStart() / batchSize)) != messagesToBeSent[sender].end()))
		{
			messagesToBeSent[sender].erase(messagesToBeSent[sender].find(make_pair(receivedMsg.processId(), receivedMsg.messageIdStart() / batchSize)));
			// Increase peersAgreement by 1
			peersAgreement[receivedMsg.processId()][receivedMsg.messageIdStart() / batchSize]++;
		}
		else
		{
			// Else do nothing, just unlock the mutexes and return the function
			mutexMsgsToBeSent[sender].unlock();
			mutexMessages[receivedMsg.processId()][(receivedMsg.messageIdStart() / batchSize) % MAX_MUTEX].unlock();
			return;
		}
		mutexMsgsToBeSent[sender].unlock();
	}
	// If this message is now deliverable (we are at the exact point where a majority of the processes have acknowledged it),
	// then try to deliver it
	if (peersAgreement[receivedMsg.processId()][receivedMsg.messageIdStart() / batchSize] >= numberOfProcess / 2 + 1 &&
		((peersAgreement[receivedMsg.processId()][receivedMsg.messageIdStart() / batchSize] - 1) * 2) <= numberOfProcess)
	{
		// First we add the message to messageBuffer, which is a priority queue that helps us with the ordering of delivery.
		// All messages that are deliverable go through the buffer to ensure FIFO.
		if (bufferDuplicateChecker[receivedMsg.processId()].insert(receivedMsg.messageIdStart() / batchSize).second)
		{
			messageBuffer[receivedMsg.processId()].push(receivedMsg);
		}

		// We deliver the next expected message if it is in the buffer, and keep checking the buffer in case that we can now deliver
		// other messages that were unblocked in the buffer because of the latest delivery.
		while (!messageBuffer[receivedMsg.processId()].empty() && messageBuffer[receivedMsg.processId()].top().messageIdStart() / batchSize == delivered[receivedMsg.processId()] + (batchSize > 1 ? 0 : 1))
		{
			bool hasPendingPast = false;

			// Loop through the past of that message
			for (uint i = 0; i < receivedMsg.past().size(); i++)
			{
				// Check if it hasn't been delivered yet
				if (receivedMsg.past()[i].second + (batchSize > 1 ? 1 : 0) > delivered[receivedMsg.past()[i].first])
				{
					hasPendingPast = true;
					// Insert to the buffer if it is not there yet
					if (bufferDuplicateChecker[receivedMsg.past()[i].first].insert(receivedMsg.past()[i].second).second)
					{
						if (VERBOSE)
						{
							printf("We are inserting to the buffer from the past \n");
						}
						std::vector<pair<int, int>> auxPast;
						int start, end;
						if (batchSize == 1)
						{
							start = (receivedMsg.past()[i].second);
							end = (receivedMsg.past()[i].second);
						}
						else
						{
							int length_message = ((numberOfMessages / batchSize == receivedMsg.past()[i].second) ? (numberOfMessages % batchSize) : (batchSize));
							start = 1 + (receivedMsg.past()[i].second * batchSize);
							end = receivedMsg.past()[i].second * batchSize + length_message;
						}
						messagePacket auxPacket{receivedMsg.past()[i].first, start, end, 0, auxPast};
						messageBuffer[receivedMsg.past()[i].first].push(auxPacket);
					}
					// Try to deliver from the message buffer respecting the FIFO order
					while (!messageBuffer[receivedMsg.past()[i].first].empty() && messageBuffer[receivedMsg.past()[i].first].top().messageIdStart() / batchSize == delivered[receivedMsg.past()[i].first] + (batchSize > 1 ? 0 : 1))
					{
						// Deliver from the past we received in the message
						if (VERBOSE)
						{
							printf("From the past: d %d %d:%d\n", receivedMsg.past()[i].first + 1, messageBuffer[receivedMsg.past()[i].first].top().messageIdStart(), messageBuffer[receivedMsg.past()[i].first].top().messageIdEnd());
						}
						// Log the delivery
						auto temp = make_tuple('d', receivedMsg.past()[i].first + 1, messageBuffer[receivedMsg.past()[i].first].top().messageIdStart(), messageBuffer[receivedMsg.past()[i].first].top().messageIdEnd());
						logs.push_back(temp);
						delivered[receivedMsg.past()[i].first]++;
						// Update peersAgreement
						peersAgreement[receivedMsg.past()[i].first][messageBuffer[receivedMsg.past()[i].first].top().messageIdStart() / batchSize] = numberOfProcess;
						// Check if it was a message from a process we depend on
						if (dependency[processId - 1].find(receivedMsg.past()[i].first) != dependency[processId - 1].end())
						{
							if (find(localPast.begin(), localPast.end(), make_pair(receivedMsg.past()[i].first, messageBuffer[receivedMsg.past()[i].first].top().messageIdStart() / batchSize)) == localPast.end())
							{
								localPast.push_back(make_pair(receivedMsg.past()[i].first, messageBuffer[receivedMsg.past()[i].first].top().messageIdStart() / batchSize));
							}
						}

						// We remove it from the buffer
						messageBuffer[receivedMsg.past()[i].first].pop();
					}
				}
			}
			// Go back to the beggining of the while in order to check the past until it is completely delivered.
			if (hasPendingPast)
			{
				continue;
			}

			// The delivery itself
			if (VERBOSE)
				printf("d %d %d:%d\n", receivedMsg.processId() + 1, messageBuffer[receivedMsg.processId()].top().messageIdStart(), messageBuffer[receivedMsg.processId()].top().messageIdEnd());
			// Log the delivery
			auto temp = make_tuple('d', receivedMsg.processId() + 1, messageBuffer[receivedMsg.processId()].top().messageIdStart(), messageBuffer[receivedMsg.processId()].top().messageIdEnd());
			logs.push_back(temp);
			delivered[receivedMsg.processId()]++;

			// Check if it was a message from a process we depend on
			if (dependency[processId - 1].find(receivedMsg.processId()) != dependency[processId - 1].end())
			{
				// Add it to our past
				if (find(localPast.begin(), localPast.end(), make_pair(receivedMsg.processId(), receivedMsg.messageIdStart() / batchSize)) == localPast.end())
				{
					localPast.push_back(make_pair(receivedMsg.processId(), receivedMsg.messageIdStart() / batchSize));
				}
			}

			// We remove it from the buffer
			messageBuffer[receivedMsg.processId()].pop();
		}
	}
	// Unlock mutex
	mutexMessages[receivedMsg.processId()][(receivedMsg.messageIdStart() / batchSize) % MAX_MUTEX].unlock();
}

// Start the broadcasting of the messages that belong to the process.
void startBroadcasting()
{

	int index;
	int currentMessage = 0;
	struct timespec sleep_time;
	batchSize = (numberOfMessages >= 10 ? numberOfMessages / 10 : 1);
	if (numberOfMessages % batchSize == 0)
	{
		index = numberOfMessages / batchSize;
	}
	else
	{
		index = numberOfMessages / batchSize + 1;
	}
	// Wait for start signal
	while (wait_for_start)
	{
		sleep_time.tv_sec = 0;
		sleep_time.tv_nsec = 1000;
		nanosleep(&sleep_time, NULL);
	}

	// Broadcast the number of messages that was passed as a parameter
	for (int i = 1; i <= index; i++)
	{
		if (stopAll)
		{
			break;
		}
		sleep_time.tv_sec = 0;
		sleep_time.tv_nsec = 100;
		nanosleep(&sleep_time, NULL);
		currentMessage++;
		// Send a message to ourselves with origin ourselves and destination ourselves,
		// handleMessage() will do the actual broadcast by inserting the message to the messagesToBeSent for each other process
		mutexPast.lock();
		messagePacket auxPacket;
		// Be careful if the batch size does not divide exactly of the number of messages, insert the correct size to the last batch
		if (i == index && numberOfMessages % batchSize != 0)
		{
			auxPacket = messagePacket{processId - 1, 1 + (currentMessage - 1) * batchSize, ((currentMessage - 1) * batchSize) + (numberOfMessages % batchSize), 0, localPast};
		}
		else
		{
			auxPacket = messagePacket{processId - 1, 1 + (currentMessage - 1) * batchSize, currentMessage * batchSize, 0, localPast};
		}
		// Handle a copy for us in order to start the peersAgreement with 1.
		handleMessage(auxPacket, processId - 1);
		localPast.clear();
		mutexPast.unlock();
		if (VERBOSE)
		{
			cout << endl;
			printf("-------- Broadcasting function -------\n");
			cout << index << endl;
			cout << processId << endl;
			printf("-------- Content of message -------\n");
			printf("Process ID is %d.\n", auxPacket.processId());
			printf("Type ID is %d.\n", auxPacket.type());
			printf("Message ID Start is %d.\n", auxPacket.messageIdStart());
			printf("Message ID End is %d.\n", auxPacket.messageIdEnd());
			printf("-------- end of broadcasting function -------\n");
		}
		// Log the broadcast
		logs.push_back(make_tuple('b', auxPacket.processId(), auxPacket.messageIdStart(), auxPacket.messageIdEnd()));
	}
}

// Starts the threads in charge of the messages
void startThreads()
{
	// Start a thread for each process that will send messages to them
	for (int i = 0; i < numberOfProcess; i++)
	{
		sleepTimers[i] = 1000;
		sendThreads[i] = thread(messageSender, i);
	}

	// Start a thread that will start broadcasting the messages from current process.
	broadcastThread = thread(startBroadcasting);
}

// Configure the UDP socket
void configureSocket(long port, const char *ipAddress)
{
	// Opens the UDP socket that will be used for the process
	if ((processSocket = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	{
		printf("Cannot open socket\n");
		exit(1);
	}
	sockaddr_in myAddress;
	memset((char *)&myAddress, 0, sizeof(myAddress));
	myAddress.sin_family = AF_INET;
	myAddress.sin_addr.s_addr = inet_addr(ipAddress);
	myAddress.sin_port = htons(port);
	// Binding to socket
	if (bind(processSocket, (sockaddr *)&myAddress, sizeof(myAddress)) < 0)
	{
		printf("Error when trying to bind listener socket\n");
		exit(1);
	}
	if (VERBOSE)
		printf("Listening on %s:%ld\n", ipAddress, port);
}

// Store the ip address and port in the correct position of sendAddresses array
// for each process to use later
void configureSendAddresses(long port, const char *ipAddress, int index)
{
	memset((char *)&sendAddresses[index], 0, sizeof(sendAddresses[index]));
	sendAddresses[index].sin_family = AF_INET;
	sendAddresses[index].sin_addr.s_addr = inet_addr(ipAddress);
	sendAddresses[index].sin_port = htons(port);
}

// Receives messages from the UDP socket
void listenToMessages()
{
	while (!stopAll)
	{
		// Buffer that will receive the message
		memset((char *)&message, 0, sizeof(message));
		struct sockaddr_in address;
		socklen_t sizeAddr = sizeof(sendAddresses);
		// Listen for a message in the UDP socket
		recvfrom(processSocket, message, 10000 * sizeof(char), 0, (struct sockaddr *)&address, &sizeAddr);
		int sender = -1;
		// Find the sender's process id (reduced by 1 in our array)
		for (int i = 0; i < numberOfProcess; i++)
		{
			if (sendAddresses[i].sin_port == address.sin_port &&
				sendAddresses[i].sin_addr.s_addr == address.sin_addr.s_addr)
			{
				sender = i;
				break;
			}
		}
		// Deserialize the message
		messagePacket receivedMessage = deserializeMessage(message);
		if (VERBOSE)
		{
			cout << endl;
			printf("-------- listening function -------\n");
			cout << processId << endl;
			printf("-------- Content of message -------\n");
			printf("Process ID is %d.\n", receivedMessage.processId());
			printf("Type ID is %d.\n", receivedMessage.type());
			printf("Message ID Start is %d.\n", receivedMessage.messageIdStart());
			printf("Message ID End is %d.\n", receivedMessage.messageIdEnd());
			printf("-------- end of listening function -------\n");
		}

		// If we received a normal message, send an acknowledgement
		if (receivedMessage.type() == 0)
		{
			messagePacket auxPacket{receivedMessage.processId(), receivedMessage.messageIdStart(), receivedMessage.messageIdEnd(), 1, receivedMessage.past()};
			// We send the acknoledgement message back to the sender
			sendMessage(sender, serializeMessage(auxPacket));
		}

		handleMessage(receivedMessage, sender);
	}
}

// Parse the command line arguments and the membership file
void parseArguments(int argc, char **argv)
{
	if (VERBOSE)
		printf("Beginning parseArguments function\n");
	// Check that we received enough parameters
	if (argc < 4)
	{
		printf("Please put all needed arguments: da proc n membership m");
		exit(1);
	}
	processId = atoi(argv[1]);
	freopen(argv[2], "r", stdin);
	numberOfMessages = atoi(argv[3]);

	// Read the number of processes
	std::cin >> numberOfProcess;

	// Read the id, address and port for each process
	for (int i = 0; i < numberOfProcess; i++)
	{
		std::string processIdentity_temp;
		int processId;
		int port_temp;

		std::cin >> processId >> processIdentity_temp >> port_temp;
		processIdentities.push_back({processIdentity_temp, port_temp});
	}
	// scanning the dependencies
	// strip previous carriage line
	string temp;
	std::getline(std::cin, temp);
	while (std::getline(std::cin, temp))
	{
		dependencies_lines.push_back(temp);
		if (VERBOSE)
			cout << "line read:#" << temp << "#" << endl;
	}

	sendAddresses = new struct sockaddr_in[numberOfProcess];
	if (VERBOSE)
	{
		printf("processId: %d\n", processId);
		for (int i = 0; i < numberOfProcess; i++)
		{
			printf("%s : ", processIdentities[i].ip_address.c_str());
			printf("%d\n", processIdentities[i].port);
		}
	}

	// Configure the sockets and store the addresses and ports for all the input we received.
	for (int i = 0; i < numberOfProcess; i++)
	{
		if (processId - 1 == i)
		{
			configureSocket(processIdentities[i].port, processIdentities[i].ip_address.c_str());
		}
		configureSendAddresses(processIdentities[i].port, processIdentities[i].ip_address.c_str(), i);
	}

	if (VERBOSE)
		printf("Ending parseArguments function\n");
}

// Read the dependencies from the membership file
void read_dependencies()
{
	if (VERBOSE)
		printf("Starting read_dependencies function\n");
	for (string lines : dependencies_lines)
	{
		int dependant_of;
		int dependant;
		stringstream ss;
		ss << lines;
		ss >> dependant;
		while (!ss.eof())
		{
			ss >> dependant_of;
			dependency[dependant - 1].insert(dependant_of - 1);
		}
		if (VERBOSE)
		{
			cout << "Process " << dependant << "depends on: ";
			for (int x : dependency[dependant - 1])
				cout << x << " ";
			cout << endl;
		}
	}
	if (VERBOSE)
		printf("Ending read_dependencies function\n");
}

// Main Function
int main(int argc, char **argv)
{
	// Fast I/O
	// ios_base::sync_with_stdio(false);
	// cin.tie(NULL);

	//set signal handlers
	signal(SIGUSR2, start);
	signal(SIGTERM, stop);
	signal(SIGINT, stop);

	// if the program is running in debug mode, write a special file of printing
	// if(VERBOSE){
	// string output_logs_file = "logs_da_proc_" + to_string(atoi(argv[1])) + ".out";
	// freopen(output_logs_file.c_str(), "w", stdout);
	// }

	// Parse arguments, including membership
	parseArguments(argc, argv);
	if (VERBOSE)
		printf("Initializing.\n");
	// Initialize application
	initializeVariables();
	// Read dependencies
	read_dependencies();

	// Wait until start signal
	while (wait_for_start)
	{
		struct timespec sleep_time;
		sleep_time.tv_sec = 0;
		sleep_time.tv_nsec = 1000;
		nanosleep(&sleep_time, NULL);
	}

	// Start listening for incoming UDP packets
	// Broadcast messages
	startThreads();

	listenToMessages();

	// Wait until stopped
	while (1)
	{
		struct timespec sleep_time;
		sleep_time.tv_sec = 1;
		sleep_time.tv_nsec = 0;
		nanosleep(&sleep_time, NULL);
	}
}
