currently, the threads each have direct access to packet queues as well as message queues.

The demuxing thread gets packets from `decoded_pktq` and places them in `demuxed_pktq`,
then the decoding thread gets packets from `demuxed_pktq` and places them back in `decoded_pktq`.
Each of these actions is associated by a messge sent in the message queue.
This is problematic because the message queue is shared and can get clogged up with irrelevant messages.
The decoupling of the message from the actual packet and the complicated communication network between
threads leads to confusing logic and unfixable bugs.

The worker threads will no longer have access to the packet queues or each other directly.
They will send messages only to the manager thread, and the manager thread will handle all data
such as packets. The manager thread also owns the framebuffer

the manager thread communicates with the main thread, recieving commands and responding with frames.

If the decode queue is running low (or has just been invalidated) the manager thread requests
a packet from the demux thread. This request comes with a target timestamp.
The demux thread does not have to return the frame with that timestamp immediately
(it can return a different frame) but it must return the target eventually if repeatedly requested.
the returned packets from the demux thread are put in the decode queue, and if it recieves the packet
with the target timestamp it will increment it.

whenever the framebuffer is empty but the decode queue is not,
the request is sent to the video thread to start decoding a new frame which includes the
packet from the decode queue.
Once the video thread is done, it sends the completed frame back to the manager thread along
with its timestamp. ("send" being used loosely here)
Only then does the manager thread check if the timestamp of the frame is correct.
(there's no point in checking sooner because proceeding frames often have to be fully
 decoded for subsequent ones to be, so "incorrect" frames must still be allowed through
 the decoder)
If the timestamp is wrong, the frame is discarded.
If it is right, the frame is kept in the framebuffer.
this timestamp is not the same as the target demux timestamp as long as the video is playing.
the manager thread must keep track of both the target demux timestamp and the target frame timestamp
seperately.

when `advance_frame()` or `seek()` is called, the main thread requests the manager thread to jump
to a different frame. It does not solicit a response from the manager thread. Generally, `advance_frame()`
will result in a new frame very quickly because it is just swapping a framebuffer, while seek() may take
longer. Either way, `get_frame()` will continue return the current frame until the new one is ready.

when `get_frame()` is called, the main thread sends a request for a valid frame to the manager thread
which must be returned immediately.

when the manager thread recieves a request to advance the frame, it swaps the framebuffer as soon as it
is full and then increments the target frame timestamp. 
In this case, the framebuffer definitely contains the next frame so it doesn't need to check that again.

when the manager recieves a request to seek, the framebuffer almost certainly doesn't contain the
right frame, so it should be immediately discarded. The decode queue can also safely be invalidated.
Finally, the target demux timestamp and target frame timestamp are both set to the seek timestamp.









