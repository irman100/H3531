10 REM H3531 PADDLE - MATRIX BRANDY WIDESCREEN GAME
20 REM ARROWS OR A/D MOVE. Q QUITS. KEEP THE BALL ALIVE.
30 MODE 71
40 CLG:CLS
50 OSCLI "REFRESH OFF"
60 ON ERROR OSCLI "REFRESH ON":REPORT:PRINT REPORT$:END
70 score%=0:lives%=3
80 px%=864:py%=90:pw%=320:ph%=28
90 bx%=1024:by%=576:br%=22:dx%=14:dy%=12
100 GCOL 0,6
110 RECTANGLE 64,64,1920,1024
120 GCOL 0,7
130 PRINT TAB(2,1);"H3531 PADDLE   ARROWS/A-D MOVE   Q QUITS"
140 PROCscore
150 PROCdraw
160 OSCLI "REFRESH"
170 REPEAT
180   k%=INKEY(0)
190   IF k%=140 OR k%=ASC("a") OR k%=ASC("A") THEN px%=px%-48
200   IF k%=141 OR k%=ASC("d") OR k%=ASC("D") THEN px%=px%+48
210   IF px%<80 THEN px%=80
220   IF px%+pw%>1968 THEN px%=1968-pw%
230   IF k%=ASC("q") OR k%=ASC("Q") THEN lives%=0
240   PROCerase
250   bx%=bx%+dx%:by%=by%+dy%
260   IF bx%-br%<80 THEN bx%=80+br%:dx%=ABS(dx%)
270   IF bx%+br%>1968 THEN bx%=1968-br%:dx%=-ABS(dx%)
280   IF by%+br%>1072 THEN by%=1072-br%:dy%=-ABS(dy%)
290   IF dy%<0 AND by%-br%<=py%+ph% AND by%+br%>=py% AND bx%>=px%-br% AND bx%<=px%+pw%+br% THEN dy%=ABS(dy%):score%=score%+1:IF score% MOD 5=0 THEN dx%=dx%+SGN(dx%):dy%=dy%+1
300   IF by%<64 THEN lives%=lives%-1:PROCscore:bx%=1024:by%=576:dy%=ABS(dy%):IF lives%>0 THEN FOR w%=1 TO 60:NEXT
310   PROCdraw
320   OSCLI "REFRESH"
330   t%=TIME:REPEAT UNTIL TIME>t%
340 UNTIL lives%=0
350 OSCLI "REFRESH ON"
360 CLG:CLS
370 PRINT "H3531 PADDLE"
380 PRINT "SCORE ";score%
390 PRINT "TYPE RUN TO PLAY AGAIN OR QUIT TO LEAVE BASIC"
400 END
410 DEF PROCerase
420 GCOL 0,0
430 RECTANGLE FILL pxold%,py%,pw%,ph%
440 CIRCLE FILL bxold%,byold%,br%+2
450 ENDPROC
460 DEF PROCdraw
470 pxold%=px%:bxold%=bx%:byold%=by%
480 GCOL 0,6
490 RECTANGLE FILL px%,py%,pw%,ph%
500 GCOL 0,7
510 CIRCLE FILL bx%,by%,br%
520 ENDPROC
530 DEF PROCscore
540 PRINT TAB(2,3);"SCORE ";score%;"   LIVES ";lives%;"   "
550 ENDPROC
