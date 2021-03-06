//
// Copyright © 2016 Associated Universities, Inc. Washington DC, USA.
//
// This file is part of vysmaw.
//
// vysmaw is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// vysmaw is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// vysmaw.  If not, see <http://www.gnu.org/licenses/>.
//
#include <iostream>
#include <array>
#include <memory>
#include <csignal>
#include <map>
#include <algorithm>
#include <vysmaw.h>

// max time to wait for message on queue
#define QUEUE_TIMEOUT_MICROSEC 100000

using namespace std;

namespace std {
	// default_delete specialization for struct vysmaw_message
	template <> struct default_delete<struct vysmaw_message> {
		void operator()(struct vysmaw_message *msg) {
			vysmaw_message_unref(msg);
		}
	};

	// default_delete specialization for struct vysmaw_configuration
	template <> struct default_delete<struct vysmaw_configuration> {
		void operator()(struct vysmaw_configuration *conf) {
			vysmaw_configuration_free(conf);
		}
	};
}

// a filter that accepts everything
void
filter(const uint8_t stations[2], uint8_t spectral_window_index,
       uint8_t stokes_index, const struct vys_spectrum_info *infos,
       uint8_t num_infos, void *user_data, bool *pass_filter)
{
	for (auto i = 0; i < num_infos; ++i)
		*pass_filter++ = true;
}

// handle sigint for user exit condition
sig_atomic_t sigint_occurred = false;
void sigint_handler(int p)
{
	sigint_occurred = true;
}

// get a message from a message queue, with a timeout to support user interrupt
// handling
unique_ptr<struct vysmaw_message>
pop(vysmaw_message_queue q)
{
	return unique_ptr<struct vysmaw_message>(
		vysmaw_message_queue_timeout_pop(q, QUEUE_TIMEOUT_MICROSEC));
}

// display counts of messages
void
show_counters(array<unsigned,VYSMAW_MESSAGE_END + 1> &counters)
{
	map<enum vysmaw_message_type,string> names;
	names[VYSMAW_MESSAGE_VALID_BUFFER] = "valid-buffer";
	names[VYSMAW_MESSAGE_DIGEST_FAILURE] = "digest-failure";
	names[VYSMAW_MESSAGE_QUEUE_OVERFLOW] = "queue-overflow";
	names[VYSMAW_MESSAGE_DATA_BUFFER_STARVATION] = "data-buffer-starvation";
	names[VYSMAW_MESSAGE_SIGNAL_BUFFER_STARVATION] = "signal-buffer-starvation";
	names[VYSMAW_MESSAGE_SIGNAL_RECEIVE_FAILURE] = "signal-receive-failure";
	names[VYSMAW_MESSAGE_RDMA_READ_FAILURE] = "rdma-read-failure";
	names[VYSMAW_MESSAGE_END] = "end";

	size_t max_name_len = 0;
	for (auto n : names)
		max_name_len = max(n.second.length(), max_name_len);

	const enum vysmaw_message_type msg_types[] = {
		VYSMAW_MESSAGE_VALID_BUFFER,
		VYSMAW_MESSAGE_DIGEST_FAILURE,
		VYSMAW_MESSAGE_QUEUE_OVERFLOW,
		VYSMAW_MESSAGE_DATA_BUFFER_STARVATION,
		VYSMAW_MESSAGE_SIGNAL_BUFFER_STARVATION,
		VYSMAW_MESSAGE_SIGNAL_RECEIVE_FAILURE,
		VYSMAW_MESSAGE_RDMA_READ_FAILURE,
		VYSMAW_MESSAGE_END
	};

	for (auto m : msg_types) {
		cout.width(max_name_len);
		cout << right << names.at(m);
		cout << ": " << counters[m] << endl;
	}
}

int
main(int argc, char *argv[])
{
	unique_ptr<struct vysmaw_configuration> config;

	// initialize vysmaw configuration
	if (argc == 2)
		config = move(
			unique_ptr<struct vysmaw_configuration>(
				vysmaw_configuration_new(argv[1])));
	else
		config = move(
			unique_ptr<struct vysmaw_configuration>(
				vysmaw_configuration_new(nullptr)));

	// one consumer, using filter()
	struct vysmaw_consumer consumer = {
		.filter = filter
	};

	// this application keeps count of the message types it receives
	array<unsigned,VYSMAW_MESSAGE_END + 1> counters;
	counters.fill(0);
	
	// catch SIGINT to exit gracefully
	bool interrupted = false;
	signal(SIGINT, sigint_handler);

	// start vysmaw client
	vysmaw_handle handle = vysmaw_start_(config.get(), 1, &consumer);

	// take messages until a VYSMAW_MESSAGE_END appears
	unique_ptr<struct vysmaw_message> message = move(pop(consumer.queue));
	while (!message || message->typ != VYSMAW_MESSAGE_END) {
		// start shutdown if requested by user
		if (sigint_occurred && !interrupted) {
			vysmaw_shutdown(handle);
			interrupted = true;
		}
		// record message type
		if (message)
			++counters[message->typ];
		// get next message
		message = move(pop(consumer.queue));
	}
	++counters[message->typ];

	// display counts of received messages
	if (interrupted) cout << endl;
	show_counters(counters);

	// display message for end condition
	switch (message->content.result.code) {
	case vysmaw_result::VYSMAW_NO_ERROR:
		cout << "ended without error" << endl;
		break;

	case vysmaw_result::VYSMAW_SYSERR:
		cout << "ended with errors" << endl
		     << message->content.result.syserr_desc;
		break;

	case vysmaw_result::VYSMAW_ERROR_BUFFPOOL:
		cout << "ended with fatal 'buffpool' error" << endl;
		break;

	default:
		break;
	}
	// release the last message and shut down the library if it hasn't already
	// been done
	message.reset();
	if (!interrupted) vysmaw_shutdown(handle);

	return 0;
}
