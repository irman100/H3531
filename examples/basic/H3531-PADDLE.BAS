10 REM H3531 PADDLE - ORIGINAL MATRIX BRANDY GRAPHICS GAME
20 REM ARROWS OR A/D MOVE. Q QUITS. KEEP THE BALL ALIVE.
30 MODE 27
40 CLS
50 OSCLI "REFRESH OFF"
60 ON ERROR OSCLI "REFRESH ON":REPORT:PRINT REPORT$:END
70 score%=0:lives%=3
80 px%=540:py%=90:pw%=200:ph%=24
90 bx%=640:by%=430:br%=18:dx%=9:dy%=8
100 GCOL 0,6
110 RECTANGLE 40,50,1200,820
120 GCOL 0,7
130 PRINT TAB(2,1);"H3531 PADDLE   ARROWS/A-D MOVE   Q QUITS"
140 PROCscore
150 PROCdraw
160 OSCLI "REFRESH"
170 REPEAT
180   k%=INKEY(0)
190   IF k%=140 OR k%=ASC("a") OR k%=ASC("A") THEN px%=px%-34
200   IF k%=141 OR k%=ASC("d") OR k%=ASC("D") THEN px%=px%+34
210   IF px%<60 THEN px%=60
220   IF px%+pw%>1220 THEN px%=1220-pw%
230   IF k%=ASC("q") OR k%=ASC("Q") THEN lives%=0
240   PROCerase
250   bx%=bx%+dx%:by%=by%+dy%
260   IF bx%-br%<55 THEN bx%=55+br%:dx%=ABS(dx%)
270   IF bx%+br%>1225 THEN bx%=1225-br%:dx%=-ABS(dx%)
280   IF by%+br%>855 THEN by%=855-br%:dy%=-ABS(dy%)
290   IF dy%<0 AND by%-br%<=py%+ph% AND by%+br%>=py% AND bx%>=px%-br% AND bx%<=px%+pw%+br% THEN dy%=ABS(dy%):score%=score%+1:IF score% MOD 5=0 THEN dx%=dx%+SGN(dx%):dy%=dy%+1
300   IF by%<45 THEN lives%=lives%-1:PROCscore:bx%=640:by%=430:dy%=ABS(dy%):IF lives%>0 THEN FOR w%=1 TO 60:NEXT
310   PROCdraw
320   OSCLI "REFRESH"
330   t%=TIME:REPEAT UNTIL TIME>t%
340 UNTIL lives%=0
350 OSCLI "REFRESH ON"
360 CLS
370 PRINT "H3531 PADDLE"
380 PRINT "SCORE ";score%
390 PRINT "TYPE RUN TO PLAY AGAIN OR QUIT TO LEAVE BASIC"
400 END
410 DEF PROCerase
420 GCOL 0,0
430 RECTANGLE pxold%,py%,pw%,ph%
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
540 OSCLI "REFRESH ON"
550 PRINT TAB(2,3);"SCORE ";score%;"   LIVES ";lives%;"   "
560 OSCLI "REFRESH OFF"
570 ENDPROC
