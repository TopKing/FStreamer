

#include "OurRTSPClient.h"

#include "player_queue.h"

// Implementation of "ourRTSPClient":

ourRTSPClient* ourRTSPClient::createNew(UsageEnvironment& env, char const* rtspURL,
					int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum) {
  return new ourRTSPClient(env, rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum);
}

ourRTSPClient::ourRTSPClient(UsageEnvironment& env, char const* rtspURL,
			     int verbosityLevel, char const* applicationName, portNumBits tunnelOverHTTPPortNum)
  : RTSPClient(env,rtspURL, verbosityLevel, applicationName, tunnelOverHTTPPortNum) {
}

ourRTSPClient::~ourRTSPClient() {
}


// Implementation of "StreamClientState":

StreamClientState::StreamClientState()
  : iter(NULL), session(NULL), subsession(NULL), streamTimerTask(NULL), duration(0.0) {
}

StreamClientState::~StreamClientState() {
  delete iter;
  if (session != NULL) {
    // We also need to delete "session", and unschedule "streamTimerTask" (if set)
    UsageEnvironment& env = session->envir(); // alias

    env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
    Medium::close(session);
  }
}


// Implementation of "DummySink":

// Even though we're not going to be doing anything with the incoming data, we still need to receive it.
// Define the size of the buffer that we'll use:
#define DUMMY_SINK_RECEIVE_BUFFER_SIZE 8192

DummySink* DummySink::createNew(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId) {
  return new DummySink(env, subsession, streamId);
}

DummySink::DummySink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId)
  : MediaSink(env),
    fSubsession(subsession) {
  fStreamId = strDup(streamId);
  fReceiveBuffer = new u_int8_t[DUMMY_SINK_RECEIVE_BUFFER_SIZE];
}

DummySink::~DummySink() {
  delete[] fReceiveBuffer;
  delete[] fStreamId;
}

void DummySink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned durationInMicroseconds) {
  DummySink* sink = (DummySink*)clientData;
  sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

// If you don't want to see debugging output for each received frame, then comment out the following line:
#define DEBUG_PRINT_EACH_RECEIVED_FRAME 1

void DummySink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
  // We've just received a frame of data.  (Optionally) print out information about it:
#ifdef DEBUG_PRINT_EACH_RECEIVED_FRAME
  if (fStreamId != NULL) envir() << "Stream \"" << fStreamId << "\"; ";
  envir() << fSubsession.mediumName() << "/" << fSubsession.codecName() << ":\tReceived " << frameSize << " bytes";
  if (numTruncatedBytes > 0) envir() << " (with " << numTruncatedBytes << " bytes truncated)";
  char uSecsStr[6+1]; // used to output the 'microseconds' part of the presentation time
  sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
  envir() << ".\tPresentation time: " << (unsigned)presentationTime.tv_sec << "." << uSecsStr;
  if (fSubsession.rtpSource() != NULL && !fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP()) {
    envir() << "!"; // mark the debugging output to indicate that this presentation time is not RTCP-synchronized
  }
  envir() << "\n";

  ////////write data to file
  //FILE *fdec = fopen("out.wav","ab");
  //if(fdec != NULL)
  //{
	 // fwrite(fReceiveBuffer,1,frameSize,fdec);
	 // fclose(fdec);
  //}

    if(pAdudio_queue != NULL && packet_queue_is_started(pAdudio_queue) )
  {
	  AVPacket packet;

      av_init_packet(&packet);
      packet.data = fReceiveBuffer;
      packet.size = frameSize;
      packet.pts = presentationTime.tv_sec;
	  packet_queue_put(pAdudio_queue, &packet);
  }

#endif
  
  // Then continue, to request the next frame of data:
  continuePlaying();
}

Boolean DummySink::continuePlaying() {
  if (fSource == NULL) return False; // sanity check (should not happen)

  // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
  fSource->getNextFrame(fReceiveBuffer, DUMMY_SINK_RECEIVE_BUFFER_SIZE,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}


H264Sink* H264Sink::createNew(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId) {
  return new H264Sink(env, subsession, streamId);
}

H264Sink::H264Sink(UsageEnvironment& env, MediaSubsession& subsession, char const* streamId)
  : MediaSink(env),
    fSubsession(subsession) {

  //unsigned int num=0;  
  //SPropRecord * sps=parseSPropParameterSets(fSubsession.fmtp_spropparametersets(),num);   
  //unsigned char start_code[4] = {0x00, 0x00, 0x00, 0x01}; 

  //  int offset = 0;
  //pPreHeader = new unsigned char[4+4+sps[0].sPropLength+sps[1].sPropLength];
  //memcpy(pPreHeader, start_code, 4);  
  //offset += 4;

  //memcpy(pPreHeader+offset,sps[0].sPropBytes,sps[0].sPropLength); 
  //offset += sps[0].sPropLength;

  //memcpy(pPreHeader+offset,start_code, 4);  
  //offset += 4;

  //memcpy(pPreHeader+offset, sps[1].sPropBytes,sps[1].sPropLength);  
  //offset += sps[1].sPropLength;

  // memcpy(pPreHeader+offset,start_code, 4);  
  // offset += 4;

  //  m_PreHeaderLen = offset;

  // memcpy(fBuffer,pPreHeader, offset);


  fStreamId = strDup(streamId);

  fBuffer[0]=fBuffer[1]=fBuffer[2]=0;
  fBuffer[3]=1; 


  pBuffer = &fBuffer[4];

}

H264Sink::~H264Sink() {

	pBuffer = NULL;
}

void H264Sink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned durationInMicroseconds) {
  H264Sink* sink = (H264Sink*)clientData;
  sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

// If you don't want to see debugging output for each received frame, then comment out the following line:
#define DEBUG_PRINT_EACH_RECEIVED_FRAME 1

void H264Sink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
  // We've just received a frame of data.  (Optionally) print out information about it:
#ifdef DEBUG_PRINT_EACH_RECEIVED_FRAME
  if (fStreamId != NULL) envir() << "Stream \"" << fStreamId << "\"; ";
  envir() << fSubsession.mediumName() << "/" << fSubsession.codecName() << ":\tReceived " << frameSize << " bytes";
  if (numTruncatedBytes > 0) envir() << " (with " << numTruncatedBytes << " bytes truncated)";
  char uSecsStr[6+1]; // used to output the 'microseconds' part of the presentation time
  sprintf(uSecsStr, "%06u", (unsigned)presentationTime.tv_usec);
  envir() << ".\tPresentation time: " << (unsigned)presentationTime.tv_sec << "." << uSecsStr;
  if (fSubsession.rtpSource() != NULL && !fSubsession.rtpSource()->hasBeenSynchronizedUsingRTCP()) {
    envir() << "!"; // mark the debugging output to indicate that this presentation time is not RTCP-synchronized
  }
  envir() << "\n";

  //////write data to file
  //FILE *fdec = fopen("out.264","ab");
  //if(fdec != NULL)
  //{
	 // fwrite(fBuffer,1,frameSize+4,fdec);
	 // fclose(fdec);
  //}



  if(pH264_queue != NULL && packet_queue_is_started(pH264_queue) )
  {
	  AVPacket packet;

      av_init_packet(&packet);
      packet.data = fBuffer;
      packet.size = frameSize +4 ;
      packet.pts = presentationTime.tv_sec;

	 
	 
	/*    switch(pBuffer[0] & 0x1f)
        {
        case 5:
		
               packet.data = fBuffer;
			   packet.size = frameSize + m_PreHeaderLen;
			
            break;
        default:
           
            packet.data = fBuffer+m_PreHeaderLen - 4;
		    packet.size = frameSize + 4;
            break;
        }
*/
	 // 	static int counter = 0;


		//char log[128];
		//sprintf(log,"#############pkt %d size =%d\n ",counter++,packet.size);

		//FILE *log1 = fopen("log1.txt","ab");
		//fwrite(log,1,128,log1);
		//fclose(log1);

	   static int counter  = 0;

	   if(counter < 2)
	   {
		   counter++;
	   }
	   else
		 packet_queue_put(pH264_queue, &packet);
  }

 

#endif
  
  // Then continue, to request the next frame of data:
  continuePlaying();
}

Boolean H264Sink::continuePlaying() {
  if (fSource == NULL) return False; // sanity check (should not happen)

  // Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
  fSource->getNextFrame(pBuffer, sizeof(fBuffer)-4,
                        afterGettingFrame, this,
                        onSourceClosure, this);
  return True;
}