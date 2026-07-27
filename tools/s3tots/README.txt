s3tots: Convert series 3 TY/TY+/TMF to mpeg2/mpeg4 transport stream format

s3tots will convert your unencrypted TY recordings into standard mpeg2/mpeg4
transport streams, in a single step.

s3tots works on all combinations of OTA, analog cable, digital cable, SD, HD,
and even tivocast, but not Amazon unbox.  (Unbox recordings seem to include
extra macrovision(?) scrambling of the video&audio streams that I'm not
interested in getting into.)

s3tots has a simple command line user interface just like tytompg.
Typical usage is simply:
	s3tots <show>.ty
Output is to <show>.ts by default.  Some switches:
-y     	     	   Allow overwrite of output file without confirmation,
		   even if it already exists
-i <file>	   Specify input ty/ty+/tmf file
-o <file>	   Output .ts to specified file
-c start,duration  Only extract .ts from start thru start+duration time.
   		   Start and end are decimal numbers, units in seconds.
-s <number>	   Skip <number> chunks on input before processing.
   		   Note: if input file is a .tmf file,
		   this option will not work correctly
-n <number>	   Process <number> chunks from input
-t 		   Omit PAT/PMT tables in generated .ts file 
		   (included by default)

Example:[CODE]
% s3tots sell.ty
s3tots: Copyright (c) 2004-2007 B.C. <bcc24x7@gmail.com>
Version 0.7, Source is sell.ty, dest is sell.ts
TY set video,audio pid: 7c0,7c1.  Audio is AC3
12956960 transport stream bytes
Recording elapsed time: 6 seconds Rate: 16.00 Mbps
%[/CODE]

I've had success playing back transport streams with most media players,
including xine, mplayer, vlc, and powerdvd.  For some streams, xine&mplayer
can screw up the a/v sync during playback, even though the sync is preserved
during the decode process.  powerdvd&vlc don't seem to share this problem.

If you're having trouble with a media player's ability to play .ts files, or
with an authoring program's support of .ts, you may want to try converting the
.ts to .mpg.  I believe the best tool for this is videoredo as it also allows
for frame accurate editing and can repair transmission problems in your
stream, all without re-transcoding the streams.

s3tots tracks and reports on continuity errors, discontinuity flags, and
warped PCR timestamps.  You likely can safely ignore any such errors, so long as you process your resulting .ts file with a program such as videoredo.  Otherwise, such errors likely are a sign that you have reception problems and you're best off re-recording your show.

Windows 32, linux-x86, linux-x86-64 binaries attached, as well as source.


Enjoy,

B.C. <bcc24x7@gmail.com>
