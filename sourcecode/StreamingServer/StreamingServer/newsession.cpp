#include "stdafx.h"
#include "newsession.h"
#include <iostream>
#include "helper.h"
#include <mmsystem.h>
#include <vlc/vlc.h>
#include <vlc/libvlc.h>
#include "music.h"

using namespace std;

HANDLE userChangeSem;
vector<string> songList;
HANDLE sessionsSem;

std::map<SOCKET, new_session*> SESSIONS;

/*******************************************************************
** Function:  AcceptThread()
**
** Date: March 22th, 2015
**
** Revisions:
**
**
** Designer: Jeff Bayntun
**
** Programmer: Jeff Bayntun
**
** Interface:
**			void AcceptThread()
**
**
** Returns:
**
** Notes: listens and waits for incoming connections indefinetely
*******************************************************************/
void AcceptThread()
{
    SOCKET Accept, temp;
    sockaddr peer;
    int peer_len;
    new_session* ns;

    if( (sessionsSem = CreateSemaphore(NULL, 1, 1, NULL)) == NULL)
    {
        printf("error creating sessionSem\n");
        return;
    }
    if( (userChangeSem = CreateSemaphore(NULL, 0, MAX_SESSIONS, NULL)) == 0)
    {
        printf("error creating userChangeSem\n");
        return;
    }

    cout << endl << "Accepting Clients" << endl;
    if(!openListenSocket(&Accept, SERVER_TCP_LISTEN_PORT) )
        return;


    cout << "Id of Accepting thread " << GetCurrentThreadId() << endl;

        while(TRUE)
       {
           peer_len = sizeof(peer);
           ZeroMemory(&peer, peer_len);

           temp = accept(Accept, &peer, &peer_len);
           ns = new new_session;
           ns->s = temp;

            getpeername(temp, &peer, &peer_len);
            sockaddr_in* s_in = (struct sockaddr_in*)&peer;
            char* temp_addr = inet_ntoa( s_in->sin_addr );

            ns->ip.assign(temp_addr);

            printf("Socket %d is connected to ", temp);
            printIP(*s_in);

            createSession(ns);
       }

    return;
}

/*******************************************************************
** Function: createSession()
**
** Date: March 22th, 2015
**
** Revisions:
**
**
** Designer: Jeff Bayntun
**
** Programmer: Jeff Bayntun
**
** Interface:
**			bool createSession(SOCKET c, char* a)
**          c: control socket for this session
**          a: ip address of client
**
**
** Returns:
**			false on failure
**
** Notes: Creates the threads and semaphores specific to this session
*******************************************************************/
bool createSession(new_session* ns)
{
    HANDLE h;
    cout << endl << "Creating a session for control socket " << ns->s << " with ip " << ns->ip << endl;

    WaitForSingleObject(sessionsSem, INFINITE);
    SESSIONS.insert(make_pair(ns->s, ns));
    ReleaseSemaphore(userChangeSem, SESSIONS.size(), 0);
    ReleaseSemaphore(sessionsSem, 1, 0);

    createWorkerThread(controlThread, &h, ns, 0);

    return true;
}


/*******************************************************************
** Function:  controlThread()
**
** Date: March 22th, 2015
**
** Revisions:
**
**
** Designer: Jeff Bayntun
**
** Programmer: Jeff Bayntun
**
** Interface:
**			DWORD WINAPI controlThread(LPVOID lpParameter)
**
**
** Returns:
**			FALSE on failure
**
** Notes: each session has its own control thread.  This is used
**  to coordinate other threads and activities
*******************************************************************/
DWORD WINAPI controlThread(LPVOID lpParameter)
{
    new_session* ns = (new_session*) lpParameter;
    SOCKET s = ns->s;

    LPSOCKET_INFORMATION SI;
    SI = createSocketInfo(s);


    LPSOCKET_INFORMATION si = SI;
  
    DWORD RecvBytes, result, flags;
    HANDLE* waitHandles = &userChangeSem;

    flags = 0;


    // send song list
    sendSongList(s);
    
    //post rcv call waiting for data
    if ( WSARecv( s, &(si->DataBuf), 1, &RecvBytes, &flags, &(si->Overlapped), controlRoutine ) == SOCKET_ERROR)
      {
         if (WSAGetLastError() != WSA_IO_PENDING)
         {
            printf("WSARecv() failed with error %d, control thread\n", WSAGetLastError());
            sessionCleanUp(si);
         }
      } 

    // get into alertable state
    while(1)
    {
        result = WaitForMultipleObjectsEx(1, waitHandles, FALSE, INFINITE, TRUE);
        if(result == WAIT_FAILED)
        {
            perror("error waiting for multiple objects");
            sessionCleanUp(si);
        }

        if (result == WAIT_IO_COMPLETION)
         {
            continue;
         }

         switch(result -  WAIT_OBJECT_0)
         {
            case 0:
                 // change in user list, access the list and send it
                updateNewUser(si->Socket);
                 break;
         default:
                //error of somekind, clean up sessions and exit
                sessionCleanUp(si);
         }
    }

    return FALSE;
}

void sendSongList(SOCKET c)
{
    if(songList.empty())
        return;

    string temp;
    ctrlMessage message;

    message.msgData = songList;
    message.type = LIBRARY_INFO;
    createControlString(message, temp);
    string to_send = "********************************************" + temp;

    sendTCPMessage(&c, to_send, DATA_BUFSIZE);

}


/*******************************************************************
** Function:  sessionCleanUp()
**
** Date: March 22th, 2015
**
** Revisions:
**
**
** Designer: Jeff Bayntun
**
** Programmer: Jeff Bayntun
**
** Interface:
**			void sessionCleanUp(LPMUSIC_SESSION m)
**          m: session struct to create semaphores for
**
**
** Returns:
**		 void
**
** Notes: closes threads, SOCKET_INFORMATIONS, and semaphores of
** the input session.
*******************************************************************/
void sessionCleanUp(LPSOCKET_INFORMATION si)
{
    printf("Session clean up for socket %d\n", si->Socket);

    deleteSocketInfo(si);

    //delete session from map
    WaitForSingleObject(sessionsSem, INFINITE);
    SESSIONS.erase(si->Socket);
    ReleaseSemaphore(sessionsSem, 1, NULL);

    //signal other sessions to send the updated userlist
    ReleaseSemaphore(userChangeSem, SESSIONS.size(), 0);

    //close this thread
    ExitThread(0);
}


void CALLBACK controlRoutine(DWORD Error, DWORD BytesTransferred, LPWSAOVERLAPPED Overlapped, DWORD InFlags)
{

    DWORD SendBytes, RecvBytes;
   DWORD Flags;
   string message_rcv;
   ctrlMessage ctrl;


   // Reference the WSAOVERLAPPED structure as a SOCKET_INFORMATION structure
   LPSOCKET_INFORMATION SI = (LPSOCKET_INFORMATION) Overlapped;

   if (Error != 0)
   {
     printf("I/O operation failed with error %d\n", Error);
   }

   if (BytesTransferred == 0) // nothing sent or rcvd...
   {
      printf("Closing control socket %d\n", SI->Socket);
   }

   if (Error != 0 || BytesTransferred == 0)
   {
       sessionCleanUp(SI);
   }

   // Check to see if the BytesRECV field equals zero. If this is so, then
   // this means a WSARecv call just completed so update the BytesRECV field
   // with the BytesTransferred value from the completed WSARecv() call.

   if (SI->BytesRECV == 0) // after a rcv
   {
      SI->BytesRECV = BytesTransferred;
      SI->BytesSEND = 0;
      //rcv = true;
      message_rcv.assign(SI->Buffer);
      parseControlString(message_rcv, &ctrl);
      switch(ctrl.type)
      {
          //someone wants to make a mic chat with this client
          case MIC_CONNECTION:
          {
              cout << "mic connection from socket " << SI->Socket << endl;
              break;
          }
          //song has been requested for unicast send to this client
          case SONG_REQUEST:
          {
              cout << "song request from socket " << SI->Socket << endl;
              break;
          }

          //song has been requested for tcp send to this client
          case SAVE_SONG:
          {
            cout << "download request from socket " << SI->Socket << endl;
            transmitSong(SI->Socket, ctrl.msgData[0]);
              break;
          }
          case END_CONNECTION:
          default:
              {

              }
      }

       SI->bytesToSend = 0; // send back same size, that is what they're expecting
   }
   else // after a send
   {
      SI->BytesSEND += BytesTransferred;
      SI->bytesToSend -= BytesTransferred;
   }

   if (SI->bytesToSend > 0) // send or continue sending
   {

      // Post another WSASend() request.
      // Since WSASend() is not gauranteed to send all of the bytes requested,
      // continue posting WSASend() calls until all received bytes are sent.

      ZeroMemory(&(SI->Overlapped), sizeof(WSAOVERLAPPED));

      SI->DataBuf.buf = SI->Buffer + SI->BytesSEND;
      SI->DataBuf.len = SI->bytesToSend;

      if (WSASend(SI->Socket, &(SI->DataBuf), 1, &SendBytes, 0,
         &(SI->Overlapped), controlRoutine) == SOCKET_ERROR)
      {
         if (WSAGetLastError() != WSA_IO_PENDING)
         {
            printf("WSASend() failed with error %d\n", WSAGetLastError());
            return;
         }
      }
   }
   else // all was sent, ready for next RECV
   {
      SI->BytesRECV = 0;
      SI->bytesToSend = 0; // just in case it got negative somehow.....

      // Now that there are no more bytes to send post another WSARecv() request.

      Flags = 0;
      ZeroMemory(&(SI->Overlapped), sizeof(WSAOVERLAPPED));
      ZeroMemory(&(SI->Buffer), sizeof(DATA_BUFSIZE));

      SI->DataBuf.len = DATA_BUFSIZE;
      SI->DataBuf.buf = SI->Buffer; // should this be zeroed??

      if (WSARecv(SI->Socket, &(SI->DataBuf), 1, &RecvBytes, &Flags,
         &(SI->Overlapped), controlRoutine) == SOCKET_ERROR)
      {
         if (WSAGetLastError() != WSA_IO_PENDING )
         {
            printf("WSARecv() failed with error %d, ctrl routine\n", WSAGetLastError());
            return;
         }
      }
   }
}

void updateNewUser(SOCKET c)
{
	ctrlMessage message;
	string temp;

	MetaData *d = new MetaData;
	fetchMetaData(d);
	//create the now playing control message
	stringstream ss;
	ss << d->title << "^" << d->artist << "^" << d->album << "^";
	message.msgData.push_back(ss.str());
    message.type = NOW_PLAYING;

    createControlString(message, temp);
     std::cout << "ACKKKKKKKKKKKKKKKKKKKKKKK !!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
    string to_send = "********************************************" + temp;

	//send the message to the client
	sendTCPMessage(&c, to_send, DATA_BUFSIZE);

	delete d;

}


void sendUserList(SOCKET c)
{
    vector<string> users;
    string temp;
    ctrlMessage message;
    new_session* ns;

    WaitForSingleObject(sessionsSem, INFINITE);
    map<const SOCKET, new_session*>::iterator &it = SESSIONS.begin();
    while(it != SESSIONS.end())
    {
        if(it->first == c)
            continue;

        ns = it->second;
        users.push_back(ns->ip);
    }
    ReleaseSemaphore(sessionsSem, 1, 0);

    if(users.empty())
        return;

    
    message.msgData = users;
    message.type = CURRENT_LISTENERS;
    createControlString(message, temp);

    string to_send = "********************************************" + temp;

    //call send function
    sendTCPMessage(&c, to_send, DATA_BUFSIZE);
}



void sendNowPlaying(string artist, string name, string album, string length)
{
	string temp;
    ctrlMessage message;
    cout << "YESSSSSSSSSSSSSSSSSSSSSSSSSSSS !!!!!!!!!!!!!!!!!!!!!!!!!" << endl;

	//create the control message
	stringstream ss;
	ss << name << "^" << artist << "^" << album << "^";
	message.msgData.push_back(ss.str());
    message.type = NOW_PLAYING;

    createControlString(message, temp);

    string to_send = "********************************************" + temp;

    //call send function
//	sendToAll(to_send);
}

void transmitSong(SOCKET s, string song)
{
    // get ns
    new_session* ns;
    HANDLE thread;

    WaitForSingleObject(sessionsSem, INFINITE);
    ns = SESSIONS.at(s);
    ReleaseSemaphore(sessionsSem, 1, 0);

    // put song in ns
    ns->song.assign(song);

    // start thread with ns as param to do the transfer.
    createWorkerThread(sendTCPSong, &thread, ns, 0);
}

DWORD WINAPI sendTCPSong(LPVOID lpParameter)
{
    new_session* ns = (new_session*) lpParameter;
    char* temp;
    string to_send;
    long file_size;

    //load file for song
    if((file_size = loadFile(ns->song.c_str(), &temp)) == -1)
    {   cout << "Error loading file " << ns->song << " for tcp send" << endl;
        return FALSE;
    }
    

    SOCKET socket;
    Sleep(2000);
    if(!openTCPSend(&socket, CLIENT_TCP_PORT, ns->ip) )
    {
        cout << "couldn't connect to client rcv" << endl;
        return FALSE;
    }

    //send song
    Sleep(500);
    sendTCPMessage(&socket, temp, file_size, DATA_BUFSIZE);

   // maybe signal control thread.....

    //exit thread
    closesocket(socket);
    return TRUE;
}