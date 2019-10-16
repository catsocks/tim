# tim

A tiny instant messenger for the terminal in a single C file I made as an exercise to learn more about sockets and terminal emulators, meant to run with most terminal emulators on most recent Linux and BSD systems.

## Usage

    Tiny instant messenger

    Usage:
        tim -l
        tim -c localhost -n ferris
        tim -L [::1]:8000 -y
        tim -c [::1]:8000 -n ferris

    Options:
        -l       Listen for a connection at any IPv4 address
        -L ADDR  Listen for a connection at a specific address
        -c ADDR  Open a connection to an address
        -n NICK  Change your default nickname
        -y       Assume yes when asked to start a conversation
        -h       Show this message

By default tim will listen for a connection at any IPv4 address at port 7171, and use your username as your nickname unless you change it:

    $ tim -l -n gopher
    tim: Talk to "ferris" from 127.0.0.1? [y/N]: y
    tim: You are now talking to ferris
    ferris: Hi!
    gopher: Bye!
    tim: You ended the conversation

Connect to a listening tim to start a conversation:

    $ tim -c 127.0.0.1 -n ferris
    tim: You are now talking to gopher
    ferris: Hi!
    gopher: Bye!
    tim: ferris ended the conversation

You will be informed of unacknowledged messages if any remains before the program exits:

    tim: Your last 1 message(s) may not have been sent

Note that IPv6 addresses must be enclosed with square brackets per [RFC 2732](https://www.ietf.org/rfc/rfc2732.txt), and that the port you choose to use to listen for connections might need to be forwarded to your computer in your router so that incoming connections from outside your local network may reach the program.

## Build

There are no dependencies or extra build flags needed, compile it in C99 mode (or later) with GNU extensions, or execute the build shell script and an executable should appear which you may copy somewhere present in your PATH environment variable.

## Protocol

The protocol is inspired by the IRC protocol and it is made up of 7 simple plain text messages meant to fit a 1kb buffer:

* `NICK <nickname>` Send nickname and request or accept a conversation
* `BUSY` Decline a conversation
* `MSG <msg id> <msg body>` Send a chat message
* `ACK <msg id>` Acknowledge a received chat message
* `PING` Test that an idle connection is still open
* `PONG` Answer PING
* `QUIT` Quit a conversation

## TODO

* Support more ANSI control sequences
* Minimize global state by improving error handling and doing away with atexit
* Clearer separation of concerns in function names
* Encryption

## Resources

* [Raw input and output](https://viewsourcecode.org/snaptoken/kilo/03.rawInputAndOutput.html) chapter of [Build Your Own Text Editor](https://viewsourcecode.org/snaptoken/kilo/index.html)
* [Beej's Guide to Network Programming](https://beej.us/guide/bgnet/)
* [OpenBSD man pages](https://man.openbsd.org/) such as [getopt(3)](https://man.openbsd.org/getopt.3) and [poll(2)](https://man.openbsd.org/poll.2)
* [ANSI control sequences](https://en.wikipedia.org/wiki/ANSI_escape_code#CSI_sequences) and [ASCII control characters](https://en.wikipedia.org/wiki/ASCII#Control_characters) on Wikipedia
* [A quick, basic guide on the IRC protocol - Macha](http://blog.initprogram.com/2010/10/14/a-quick-basic-primer-on-the-irc-protocol/)
