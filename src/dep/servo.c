/*-
 * Copyright (c) 2011-2012 George V. Neville-Neil,
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen,
 *                         Inaqui Delgado,
 *                         Rick Ratzel,
 *                         National Instruments.
 * Copyright (c) 2009-2010 George V. Neville-Neil, 
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen
 *
 * Copyright (c) 2005-2008 Kendall Correll, Aidan Williams
 *
 * All Rights Reserved
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file   servo.c
 * @date   Tue Jul 20 16:19:19 2010
 * 
 * @brief  Code which implements the clock servo in software.
 * 
 * 
 */

#include "../ptpd.h"

void
reset_operator_messages(RunTimeOpts * rtOpts, PtpClock * ptpClock)
{
	ptpClock->warned_operator_slow_slewing = 0;
	ptpClock->warned_operator_fast_slewing = 0;

	//ptpClock->seen_servo_stable_first_time = FALSE;
}

void
initClock(RunTimeOpts * rtOpts, PtpClock * ptpClock)
{
	DBG("initClock\n");


/* do not reset frequency here - restoreDrift will do it if necessary */
#if !defined(__APPLE__)
	ptpClock->servo.observedDrift = 0;
#endif /* apple */

	/* clear vars */
	ptpClock->owd_filt.s_exp = 0;	/* clears one-way delay filter */

	/* clean more original filter variables */
	clearTime(&ptpClock->offsetFromMaster);
	clearTime(&ptpClock->meanPathDelay);
	clearTime(&ptpClock->delaySM);
	clearTime(&ptpClock->delayMS);

	ptpClock->ofm_filt.y           = 0;
	ptpClock->ofm_filt.nsec_prev   = -1; /* AKB: -1 used for non-valid nsec time */
	ptpClock->ofm_filt.nsec_prev   = 0;

	ptpClock->owd_filt.s_exp       = 0;  /* clears one-way delay filter */
	rtOpts->offset_first_updated   = FALSE;

	ptpClock->char_last_msg='I';

	reset_operator_messages(rtOpts, ptpClock);

	/* For Hybrid mode */
	ptpClock->masterAddr = 0;


}

void
updateDelay(one_way_delay_filter * owd_filt, RunTimeOpts * rtOpts, PtpClock * ptpClock, TimeInternal * correctionField)
{

	/* updates paused, leap second pending - do nothing */
	if(ptpClock->leapSecondInProgress)
		return;

	DBGV("updateDelay\n");



	/* todo: do all intermediate calculations on temp vars */
	TimeInternal prev_meanPathDelay = ptpClock->meanPathDelay;

	ptpClock->char_last_msg = 'D';

	{
		//perform basic checks, using local variables only
		TimeInternal slave_to_master_delay;
	
		/* calc 'slave_to_master_delay' */
		subTime(&slave_to_master_delay, &ptpClock->delay_req_receive_time,
			&ptpClock->delay_req_send_time);

		if (rtOpts->maxDelay && /* If maxDelay is 0 then it's OFF */
		    rtOpts->offset_first_updated) {

			if ((slave_to_master_delay.nanoseconds < 0) &&
			    (abs(slave_to_master_delay.nanoseconds) > rtOpts->maxDelay)) {
				INFO("updateDelay aborted, "
				     "delay (sec: %d ns: %d) is negative\n",
				     slave_to_master_delay.seconds,
				     slave_to_master_delay.nanoseconds);
				INFO("send (sec: %d ns: %d)\n",
				     ptpClock->delay_req_send_time.seconds,
				     ptpClock->delay_req_send_time.nanoseconds);
				INFO("recv (sec: %d n	s: %d)\n",
				     ptpClock->delay_req_receive_time.seconds,
				     ptpClock->delay_req_receive_time.nanoseconds);
				goto display;
			}

			if (slave_to_master_delay.seconds && rtOpts->maxDelay) {
				INFO("updateDelay aborted, delay %d.%d greater than 1 second\n",
				     slave_to_master_delay.seconds,
				     slave_to_master_delay.nanoseconds);
				if (rtOpts->displayPackets)
					msgDump(ptpClock);
				goto display;
			}

			if (slave_to_master_delay.nanoseconds > rtOpts->maxDelay) {
				INFO("updateDelay aborted, delay %d greater than "
				     "administratively set maximum %d\n",
				     slave_to_master_delay.nanoseconds, 
				     rtOpts->maxDelay);
				if (rtOpts->displayPackets)
					msgDump(ptpClock);
				goto display;
			}
		}
	}

	/*
	 * The packet has passed basic checks, so we'll:
	 *   - update the global delaySM variable
	 *   - calculate a new filtered MPD
	 */
	if (rtOpts->offset_first_updated) {
		Integer16 s;

		/*
		 * calc 'slave_to_master_delay' (Master to Slave delay is
		 * already computed in updateOffset )
		 */

		DBG("==> UpdateDelay():   %s\n",
			dump_TimeInternal2("Req_RECV:", &ptpClock->delay_req_receive_time,
			"Req_SENT:", &ptpClock->delay_req_send_time));
		
#ifdef PTPD_STATISTICS
	if (rtOpts->delaySMOutlierFilterEnabled) {
		subTime(&ptpClock->rawDelaySM, &ptpClock->delay_req_receive_time, 
			&ptpClock->delay_req_send_time);
		if(!isDoublePeircesOutlier(ptpClock->delaySMRawStats, timeInternalToDouble(&ptpClock->rawDelaySM), rtOpts->delaySMOutlierFilterThreshold)) {
			ptpClock->delaySM = ptpClock->rawDelaySM;
			ptpClock->delaySMoutlier = FALSE;
		} else {
			ptpClock->delaySMoutlier = TRUE;
			ptpClock->counters.delaySMOutliersFound++;
			if (!rtOpts->delaySMOutlierFilterDiscard)  {
				ptpClock->delaySM = doubleToTimeInternal(ptpClock->delaySMFiltered->mean);
			} else {
				    goto statistics;
			}
		}
	} else {
		subTime(&ptpClock->delaySM, &ptpClock->delay_req_receive_time, 
			&ptpClock->delay_req_send_time);
	}
#else
		subTime(&ptpClock->delaySM, &ptpClock->delay_req_receive_time, 
			&ptpClock->delay_req_send_time);
#endif
		/* update 'one_way_delay' */
		addTime(&ptpClock->meanPathDelay, &ptpClock->delaySM, 
			&ptpClock->delayMS);

		/* Substract correctionField */
		subTime(&ptpClock->meanPathDelay, &ptpClock->meanPathDelay, 
			correctionField);

		/* Compute one-way delay */
		div2Time(&ptpClock->meanPathDelay);
		
		if (ptpClock->meanPathDelay.seconds) {
			DBG("update delay: cannot filter with large OFM, "
				"clearing filter\n");
			INFO("Servo: Ignoring delayResp because of large OFM\n");
			
			owd_filt->s_exp = owd_filt->nsec_prev = 0;
			/* revert back to previous value */
			ptpClock->meanPathDelay = prev_meanPathDelay;

			goto display;
		}


		if(ptpClock->meanPathDelay.nanoseconds < 0){
			DBG("update delay: found negative value for OWD, "
			    "so ignoring this value: %d\n",
				ptpClock->meanPathDelay.nanoseconds);
			/* revert back to previous value */
			ptpClock->meanPathDelay = prev_meanPathDelay;
#ifdef PTPD_STATISTICE
			goto statistics;
#else
			goto display;
#endif /* PTPD_STATISTICS */
		}

		/* avoid overflowing filter */
		s = rtOpts->s;
		while (abs(owd_filt->y) >> (31 - s))
			--s;

		/* crank down filter cutoff by increasing 's_exp' */
		if (owd_filt->s_exp < 1)
			owd_filt->s_exp = 1;
		else if (owd_filt->s_exp < 1 << s)
			++owd_filt->s_exp;
		else if (owd_filt->s_exp > 1 << s)
			owd_filt->s_exp = 1 << s;

		/* filter 'meanPathDelay' */
		float fy =
			(double)((owd_filt->s_exp - 1.0) *
			owd_filt->y / (owd_filt->s_exp + 0.0) +
			(ptpClock->meanPathDelay.nanoseconds / 2.0 + 
			 owd_filt->nsec_prev / 2.0) / (owd_filt->s_exp + 0.0));

		owd_filt->nsec_prev = ptpClock->meanPathDelay.nanoseconds;

		owd_filt->y = round(fy);

		ptpClock->meanPathDelay.nanoseconds = owd_filt->y;

/* Update relevant statistics containers, feed outlier filter thresholds etc. */
#ifdef PTPD_STATISTICS
statistics:
                        if (rtOpts->delaySMOutlierFilterEnabled) {
                            double dDelaySM = timeInternalToDouble(&ptpClock->rawDelaySM);
                            /* If this is an outlier, bring it by a factor closer to mean before allowing to influence stdDev */
                            if(ptpClock->delaySMoutlier) {
				/* Allow [weight] * [deviation from mean] to influence std dev in the next outlier checks */
                            DBG("DelaySM outlier: %.09f\n", dDelaySM);
			    if((rtOpts->calibrationDelay<1) || ptpClock->isCalibrated)
                            dDelaySM = ptpClock->delaySMRawStats->meanContainer->mean + rtOpts->delaySMOutlierWeight * ( dDelaySM - ptpClock->delaySMRawStats->meanContainer->mean);
                            } 
                                        feedDoubleMovingStdDev(ptpClock->delaySMRawStats, dDelaySM);
                                        feedDoubleMovingMean(ptpClock->delaySMFiltered, timeInternalToDouble(&ptpClock->delaySM));
                                }
                        feedDoublePermanentStdDev(&ptpClock->slaveStats.owdStats, timeInternalToDouble(&ptpClock->meanPathDelay));
#endif


		DBGV("delay filter %d, %d\n", owd_filt->y, owd_filt->s_exp);
	} else {
		INFO("Ignoring delayResp because we didn't receive any sync yet\n");
	}


display:
	logStatistics(rtOpts, ptpClock);

}

void
updatePeerDelay(one_way_delay_filter * owd_filt, RunTimeOpts * rtOpts, PtpClock * ptpClock, TimeInternal * correctionField, Boolean twoStep)
{
	Integer16 s;

	/* updates paused, leap second pending - do nothing */
	if(ptpClock->leapSecondInProgress)
		return;


	DBGV("updatePeerDelay\n");

	if (twoStep) {
		/* calc 'slave_to_master_delay' */
		subTime(&ptpClock->pdelayMS, 
			&ptpClock->pdelay_resp_receive_time, 
			&ptpClock->pdelay_resp_send_time);
		subTime(&ptpClock->pdelaySM, 
			&ptpClock->pdelay_req_receive_time, 
			&ptpClock->pdelay_req_send_time);

		/* update 'one_way_delay' */
		addTime(&ptpClock->peerMeanPathDelay, 
			&ptpClock->pdelayMS, 
			&ptpClock->pdelaySM);

		/* Substract correctionField */
		subTime(&ptpClock->peerMeanPathDelay, 
			&ptpClock->peerMeanPathDelay, correctionField);

		/* Compute one-way delay */
		div2Time(&ptpClock->peerMeanPathDelay);
	} else {
		/* One step clock */

		subTime(&ptpClock->peerMeanPathDelay, 
			&ptpClock->pdelay_resp_receive_time, 
			&ptpClock->pdelay_req_send_time);

		/* Substract correctionField */
		subTime(&ptpClock->peerMeanPathDelay, 
			&ptpClock->peerMeanPathDelay, correctionField);

		/* Compute one-way delay */
		div2Time(&ptpClock->peerMeanPathDelay);
	}

	if (ptpClock->peerMeanPathDelay.seconds) {
		/* cannot filter with secs, clear filter */
		owd_filt->s_exp = owd_filt->nsec_prev = 0;
		return;
	}
	/* avoid overflowing filter */
	s = rtOpts->s;
	while (abs(owd_filt->y) >> (31 - s))
		--s;

	/* crank down filter cutoff by increasing 's_exp' */
	if (owd_filt->s_exp < 1)
		owd_filt->s_exp = 1;
	else if (owd_filt->s_exp < 1 << s)
		++owd_filt->s_exp;
	else if (owd_filt->s_exp > 1 << s)
		owd_filt->s_exp = 1 << s;

	/* filter 'meanPathDelay' */
	owd_filt->y = (owd_filt->s_exp - 1) * 
		owd_filt->y / owd_filt->s_exp +
		(ptpClock->peerMeanPathDelay.nanoseconds / 2 + 
		 owd_filt->nsec_prev / 2) / owd_filt->s_exp;

	owd_filt->nsec_prev = ptpClock->peerMeanPathDelay.nanoseconds;
	ptpClock->peerMeanPathDelay.nanoseconds = owd_filt->y;

	DBGV("delay filter %d, %d\n", owd_filt->y, owd_filt->s_exp);
}

void
updateOffset(TimeInternal * send_time, TimeInternal * recv_time,
    offset_from_master_filter * ofm_filt, RunTimeOpts * rtOpts, PtpClock * ptpClock, TimeInternal * correctionField)
{

	DBGV("UTCOffset: %d | leap 59: %d |  leap61: %d\n", 
	     ptpClock->timePropertiesDS.currentUtcOffset,ptpClock->timePropertiesDS.leap59,ptpClock->timePropertiesDS.leap61);
        /* updates paused, leap second pending - do nothing */
        if(ptpClock->leapSecondInProgress)
		return;

	DBGV("==> updateOffset\n");

	{
	//perform basic checks, using only local variables
	TimeInternal master_to_slave_delay;

	/* calc 'master_to_slave_delay' */
	subTime(&master_to_slave_delay, recv_time, send_time);

	if (rtOpts->maxDelay) { /* If maxDelay is 0 then it's OFF */
		if (master_to_slave_delay.seconds && rtOpts->maxDelay) {
			INFO("updateOffset aborted, delay greater than 1"
			     " second.\n");
			/* msgDump(ptpClock); */
			return;
		}

		if (master_to_slave_delay.nanoseconds > rtOpts->maxDelay) {
			INFO("updateOffset aborted, delay %d greater than "
			     "administratively set maximum %d\n",
			     master_to_slave_delay.nanoseconds, 
			     rtOpts->maxDelay);
			/* msgDump(ptpClock); */
			return;
		}
	}
	}

	// used for stats feedback 
	ptpClock->char_last_msg='S';

	/*
	 * The packet has passed basic checks, so we'll:
	 *   - update the global delayMS variable
	 *   - calculate a new filtered OFM
	 */
#ifdef PTPD_STATISTICS
	if (rtOpts->delayMSOutlierFilterEnabled) {
		subTime(&ptpClock->rawDelayMS, recv_time, send_time);
		if(!isDoublePeircesOutlier(ptpClock->delayMSRawStats, timeInternalToDouble(&ptpClock->rawDelayMS), rtOpts->delayMSOutlierFilterThreshold)) {
			ptpClock->delayMSoutlier = FALSE;
			ptpClock->delayMS = ptpClock->rawDelayMS;
		} else {
			ptpClock->delayMSoutlier = TRUE;
			ptpClock->counters.delayMSOutliersFound++;
			if(!rtOpts->delayMSOutlierFilterDiscard)
			ptpClock->delayMS = doubleToTimeInternal(ptpClock->delayMSFiltered->mean);
		}
	} else {
		subTime(&ptpClock->delayMS, recv_time, send_time);
	}
#else
	/* Used just for End to End mode. */
	subTime(&ptpClock->delayMS, recv_time, send_time);
#endif

	/* Take care about correctionField */
	subTime(&ptpClock->delayMS,
		&ptpClock->delayMS, correctionField);

#ifdef PTPD_STATISTICS
#endif

	/* update 'offsetFromMaster' */
	if (ptpClock->delayMechanism == P2P) {
		subTime(&ptpClock->offsetFromMaster, 
			&ptpClock->delayMS, 
			&ptpClock->peerMeanPathDelay);
	/* (End to End mode or disabled - if disabled, meanpath delay is zero) */
	} else if (ptpClock->delayMechanism == E2E ||
	    ptpClock->delayMechanism == DELAY_DISABLED ) {

		subTime(&ptpClock->offsetFromMaster, 
			&ptpClock->delayMS, 
			&ptpClock->meanPathDelay);
	}

	if (ptpClock->offsetFromMaster.seconds) {
		/* cannot filter with secs, clear filter */
		ofm_filt->nsec_prev = 0;
		rtOpts->offset_first_updated = TRUE;
		return;
	}

	/* filter 'offsetFromMaster' */
	ofm_filt->y = ptpClock->offsetFromMaster.nanoseconds / 2 + 
		ofm_filt->nsec_prev / 2;
	ofm_filt->nsec_prev = ptpClock->offsetFromMaster.nanoseconds;
	ptpClock->offsetFromMaster.nanoseconds = ofm_filt->y;

	DBGV("offset filter %d\n", ofm_filt->y);

	/*
	 * Offset must have been computed at least one time before 
	 * computing end to end delay
	 */
		rtOpts->offset_first_updated = TRUE;
}

void
servo_perform_clock_step(RunTimeOpts * rtOpts, PtpClock * ptpClock)
{
	if(rtOpts->noAdjust){
		WARNING("Could not step clock - clock adjustment disabled\n");
		return;
	}

	TimeInternal oldTime, newTime;
	/*No need to reset the frequency offset: if we're far off, it will quickly get back to a high value */
	getTime(&oldTime);
	subTime(&newTime, &oldTime, &ptpClock->offsetFromMaster);

	setTime(&newTime);
	initClock(rtOpts, ptpClock);
	if(ptpClock->clockQuality.clockClass > 127)
		restoreDrift(ptpClock, rtOpts, TRUE);
	ptpClock->servo.runningMaxOutput = FALSE;
	toState(PTP_FAULTY, rtOpts, ptpClock);		/* make a full protocol reset */

	/* Major time change - need to inform utmp / wtmp */
	if(oldTime.seconds != newTime.seconds) {

/* Add the old time entry to utmp/wtmp */

/* About as long as the ntpd implementation, but not any less ugly */

#ifdef HAVE_UTMPX_H
		struct utmpx utx;
	memset(&utx, 0, sizeof(utx));
		strncpy(utx.ut_user, "date", sizeof(utx.ut_user));
#ifndef OTIME_MSG
		strncpy(utx.ut_line, "|", sizeof(utx.ut_line));
#else
		strncpy(utx.ut_line, OTIME_MSG, sizeof(utx_ut.line));
#endif /* OTIME_MSG */
#ifdef OLD_TIME
		utx.ut_tv.tv_sec = oldTime.seconds;
		utx.ut_tv.tv_usec = oldTime.nanoseconds / 1000;
		utx.ut_type = OLD_TIME;
#else /* no ut_type */
		utx.ut_time = oldTime.seconds;
#endif /* OLD_TIME */

/* ======== BEGIN  OLD TIME EVENT - UTMPX / WTMPX =========== */
#ifdef HAVE_UTMPXNAME
		utmpxname("/var/log/utmp");
#endif /* HAVE_UTMPXNAME */
		setutxent();
		pututxline(&utx);
		endutxent();
#ifdef HAVE_UPDWTMPX
		updwtmpx("/var/log/wtmp", &utx);
#endif /* HAVE_IPDWTMPX */
/* ======== END    OLD TIME EVENT - UTMPX / WTMPX =========== */

#else /* NO UTMPX_H */

#ifdef HAVE_UTMP_H
		struct utmp ut;
		memset(&ut, 0, sizeof(ut));
		strncpy(ut.ut_name, "date", sizeof(ut.ut_name));
#ifndef OTIME_MSG
		strncpy(ut.ut_line, "|", sizeof(ut.ut_line));
#else
		strncpy(ut.ut_line, OTIME_MSG, sizeof(ut.ut_line));
#endif /* OTIME_MSG */

#ifdef OLD_TIME
		ut.ut_tv.tv_sec = oldTime.seconds;
		ut.ut_tv.tv_usec = oldTime.nanoseconds / 1000;
		ut.ut_type = OLD_TIME;
#else /* no ut_type */
		ut.ut_time = oldTime.seconds;
#endif /* OLD_TIME */

/* ======== BEGIN  OLD TIME EVENT - UTMP / WTMP =========== */
		utmpname(UTMP_FILE);
		setutent();
		pututline(&ut);
		endutent();
		utmpname(WTMP_FILE);
		setutent();
		pututline(&ut);
		endutent();
/* ======== END    OLD TIME EVENT - UTMP / WTMP =========== */

#endif /* HAVE_UTMP_H */
#endif /* HAVE_UTMPX_H */

/* Add the new time entry to utmp/wtmp */

#ifdef HAVE_UTMPX_H
		memset(&utx, 0, sizeof(utx));
		strncpy(utx.ut_user, "date", sizeof(utx.ut_user));
#ifndef NTIME_MSG
		strncpy(utx.ut_line, "}", sizeof(utx.ut_line));
#else
		strncpy(utx.ut_line, NTIME_MSG, sizeof(utx_ut.line));
#endif /* NTIME_MSG */
#ifdef NEW_TIME
		utx.ut_tv.tv_sec = newTime.seconds;
		utx.ut_tv.tv_usec = newTime.nanoseconds / 1000;
		utx.ut_type = NEW_TIME;
#else /* no ut_type */
		utx.ut_time = newTime.seconds;
#endif /* NEW_TIME */

/* ======== BEGIN  NEW TIME EVENT - UTMPX / WTMPX =========== */
#ifdef HAVE_UTMPXNAME
		utmpxname("/var/log/utmp");
#endif /* HAVE_UTMPXNAME */
		setutxent();
		pututxline(&utx);
		endutxent();
#ifdef HAVE_UPDWTMPX
		updwtmpx("/var/log/wtmp", &utx);
#endif /* HAVE_UPDWTMPX */
/* ======== END    NEW TIME EVENT - UTMPX / WTMPX =========== */

#else /* NO UTMPX_H */

#ifdef HAVE_UTMP_H
		memset(&ut, 0, sizeof(ut));
		strncpy(ut.ut_name, "date", sizeof(ut.ut_name));
#ifndef NTIME_MSG
		strncpy(ut.ut_line, "}", sizeof(ut.ut_line));
#else
		strncpy(ut.ut_line, NTIME_MSG, sizeof(ut.ut_line));
#endif /* NTIME_MSG */
#ifdef NEW_TIME
		ut.ut_tv.tv_sec = newTime.seconds;
		ut.ut_tv.tv_usec = newTime.nanoseconds / 1000;
		ut.ut_type = NEW_TIME;
#else /* no ut_type */
		ut.ut_time = newTime.seconds;
#endif /* NEW_TIME */

/* ======== BEGIN  NEW TIME EVENT - UTMP / WTMP =========== */
		utmpname(UTMP_FILE);
		setutent();
		pututline(&ut);
		endutent();
		utmpname(WTMP_FILE);
		setutent();
		pututline(&ut);
		endutent();
/* ======== END    NEW TIME EVENT - UTMP / WTMP =========== */

#endif /* HAVE_UTMP_H */
#endif /* HAVE_UTMPX_H */
	}


}

#ifndef PTPD_DOUBLE_SERVO
void
warn_operator_fast_slewing(RunTimeOpts * rtOpts, PtpClock * ptpClock, Integer32 adj)
#else
void
warn_operator_fast_slewing(RunTimeOpts * rtOpts, PtpClock * ptpClock, double adj)
#endif /* PTPD_DOUBLE_SERVO */
{
	if(ptpClock->warned_operator_fast_slewing == 0){
		if ((adj >= rtOpts->servoMaxPpb) || ((adj <= -rtOpts->servoMaxPpb))){
			ptpClock->warned_operator_fast_slewing = 1;

			NOTICE("Servo: Going to slew the clock with the maximum frequency adjustment\n");
		}
	}

}

void
warn_operator_slow_slewing(RunTimeOpts * rtOpts, PtpClock * ptpClock )
{
	if(ptpClock->warned_operator_slow_slewing == 0){
		ptpClock->warned_operator_slow_slewing = 1;
		ptpClock->warned_operator_fast_slewing = 1;

		/* rule of thumb: at tick rate 10000, slewing at the maximum speed took 0.5ms per second */
		float estimated = (((abs(ptpClock->offsetFromMaster.seconds)) + 0.0) * 2.0 * 1000.0 / 3600.0);


		ALERT("Servo: %d seconds offset detected, will take %.1f hours to slew\n",
			ptpClock->offsetFromMaster.seconds,
			estimated
		);
		
	}
}

/*
 * this is a wrapper around adjFreq to abstract extra operations
 */
#if !defined(__APPLE__)

#ifndef PTPD_DOUBLE_SERVO

void
adjFreq_wrapper(RunTimeOpts * rtOpts, PtpClock * ptpClock, Integer32 adj)
{
    if (rtOpts->noAdjust){
		DBGV("adjFreq2: noAdjust on, returning\n");
		return;
	}

	// call original adjtime
	DBG2("     adjFreq2: call adjfreq to %d us \n", adj / DBG_UNIT);
	adjFreq(adj);

	warn_operator_fast_slewing(rtOpts, ptpClock, adj);
}

#else

void
adjFreq_wrapper(RunTimeOpts * rtOpts, PtpClock * ptpClock, double adj)
{
    if (rtOpts->noAdjust){
		DBGV("adjFreq2: noAdjust on, returning\n");
		return;
	}

	// call original adjtime
	DBG2("     adjFreq2: call adjfreq to %.09f us \n", adj / DBG_UNIT);
	adjFreq(adj);

	warn_operator_fast_slewing(rtOpts, ptpClock, adj);
}


#endif /* PTPD_DOUBLE_SERVO */

#endif /* __APPLE__ */

void
updateClock(RunTimeOpts * rtOpts, PtpClock * ptpClock)
{

	/* updates paused, leap second pending - do nothing */
        if(ptpClock->leapSecondInProgress)
            return;
	DBGV("==> updateClock\n");

	if(ptpClock->panicMode) {
	    DBG("Panic mode - skipping updateClock");
	}



/*
if(rtOpts->delayMSOutlierFilterEnabled && rtOpts->delayMSOutlierFilterDiscard && ptpClock->delayMSoutlier)
	    goto display;
*/
	if (rtOpts->maxReset) { /* If maxReset is 0 then it's OFF */
		if (ptpClock->offsetFromMaster.seconds && rtOpts->maxReset) {
			INFO("updateClock aborted, offset greater than 1"
			     " second.");
			if (rtOpts->displayPackets)
				msgDump(ptpClock);
			goto display;
		}

		if (ptpClock->offsetFromMaster.nanoseconds > rtOpts->maxReset) {
			INFO("updateClock aborted, offset %d greater than "
			     "administratively set maximum %d\n",
			     ptpClock->offsetFromMaster.nanoseconds, 
			     rtOpts->maxReset);
			if (rtOpts->displayPackets)
				msgDump(ptpClock);
			goto display;
		}
	}

	if (ptpClock->offsetFromMaster.seconds) {
		/* if secs, reset clock or set freq adjustment to max */
		
		/* 
		  if offset from master seconds is non-zero, then this is a "big jump:
		  in time.  Check Run Time options to see if we will reset the clock or
		  set frequency adjustment to max to adjust the time
		*/

		/*
		 * noAdjust     = cannot do any change to clock
		 * noResetClock = if can change the clock, can we also step it?
		 */
		if (!rtOpts->noAdjust) {


	if(rtOpts->enablePanicMode && !ptpClock->panicOver) {

		if(ptpClock->panicMode)
		    goto display;

		if(ptpClock->panicOver) {
		    ptpClock->panicMode = FALSE;
		    ptpClock->panicOver = FALSE;
#ifdef PTPD_STATISTICS
		    ptpClock->isCalibrated = FALSE;
#endif /* PTPD_STATISTICS */
		    goto display;
		}

		CRITICAL("Offset above 1 second - entering panic mode\n");

		ptpClock->panicMode = TRUE;
		ptpClock->panicModeTimeLeft = 2 * rtOpts->panicModeDuration;
		timerStart(PANIC_MODE_TIMER, 30, ptpClock->itimer);

#ifdef PTPD_NTPDC

/* Trigger NTP failover as part of panic mode */

if(rtOpts->ntpOptions.enableEngine && rtOpts->panicModeNtp) {

                                /* We have a timeout defined */
                                if(rtOpts->ntpOptions.failoverTimeout) {
                                        DBG("NTP failover timer started - panic mode\n");
                                        timerStart(NTPD_FAILOVER_TIMER,
                                                    rtOpts->ntpOptions.failoverTimeout,
                                                    ptpClock->itimer);
                                /* Fail over to NTP straight away */
                                } else {
                                        DBG("Initiating NTP failover\n");
                                        ptpClock->ntpControl.isRequired = TRUE;
                                        ptpClock->ntpControl.isFailOver = TRUE;
                                        if(!ntpdControl(&rtOpts->ntpOptions, &ptpClock->ntpControl, FALSE))
                                                DBG("PANIC MODE instant NTP failover - could not fail over\n");
                                }

}

#endif
		goto display;

	}

			if(rtOpts->enablePanicMode) {
				if(ptpClock->panicOver)
					CRITICAL("Panic mode timeout - accepting current offset. Clock will jump\n");
				ptpClock->panicOver = FALSE;
				timerStop(PANIC_MODE_TIMER, ptpClock->itimer);

#ifdef PTPD_NTPDC
/* Exiting ntp failover - getting out of panic mode */
	if(rtOpts->ntpOptions.enableEngine && rtOpts->panicModeNtp) {
                            timerStop(NTPD_FAILOVER_TIMER, ptpClock->itimer);
			    ptpClock->ntpControl.isRequired = FALSE;
			    ptpClock->ntpControl.isFailOver = FALSE;
                            if(!ntpdControl(&rtOpts->ntpOptions, &ptpClock->ntpControl, FALSE))
                                DBG("NTPdcontrol - could not return from NTP panic mode\n");
	}
#endif /* PTPD_NTPDC */

			}
			if (!rtOpts->noResetClock) {
				servo_perform_clock_step(rtOpts, ptpClock);
			} else {
#if !defined(__APPLE__)
				if(ptpClock->offsetFromMaster.nanoseconds > 0)
				    ptpClock->servo.observedDrift = rtOpts->servoMaxPpb;
				else
				    ptpClock->servo.observedDrift = -rtOpts->servoMaxPpb;
				warn_operator_slow_slewing(rtOpts, ptpClock);
				adjFreq_wrapper(rtOpts, ptpClock, -ptpClock->servo.observedDrift);
				/* its not clear how the APPLE case works for large jumps */
#endif /* __APPLE__ */
			}
		}
	} else {

	    if(rtOpts->enablePanicMode && (ptpClock->panicMode || ptpClock->panicOver)) {
		    ptpClock->panicMode = FALSE;
		    ptpClock->panicOver = FALSE;
		    timerStop(PANIC_MODE_TIMER, ptpClock->itimer);
		    NOTICE("Offset below 1 second again: exiting panic mode\n");
#ifdef PTPD_NTPDC
/* exiting ntp failover - panic mode over */
	if(rtOpts->ntpOptions.enableEngine && rtOpts->panicModeNtp) {
                            timerStop(NTPD_FAILOVER_TIMER, ptpClock->itimer);
			    ptpClock->ntpControl.isRequired = FALSE;
			    ptpClock->ntpControl.isFailOver = FALSE;
                            if(!ntpdControl(&rtOpts->ntpOptions, &ptpClock->ntpControl, FALSE))
                                DBG("NTPdcontrol - could not return from NTP panic mode\n");
	}
#endif /* PTPD_NTPDC */

	    }
/* If the last delayMS was an outlier and filter action is discard, skip servo run */
#ifdef PTPD_STATISTICS
if(rtOpts->delayMSOutlierFilterEnabled && rtOpts->delayMSOutlierFilterDiscard && ptpClock->delayMSoutlier)
	    goto statistics;
#endif /* PTPD_STATISTICS */

#if defined(__APPLE__)
			adjTime(ptpClock->offsetFromMaster.nanoseconds);
#else

#ifdef PTPD_STATISTICS
	/* if statistics are enabled, only run the servo if we are calibrted - if calibration delay configured */
	if(!rtOpts->calibrationDelay || ptpClock->isCalibrated)
#endif /*PTPD_STATISTICS */
		/* Adjust the clock first -> the PI controller runs here */
		adjFreq_wrapper(rtOpts, ptpClock, runPIservo(&ptpClock->servo, ptpClock->offsetFromMaster.nanoseconds));
		/* Unset STA_UNSYNC */
		unsetTimexFlags(STA_UNSYNC, TRUE);
		/* "Tell" the clock about maxerror, esterror etc. */
		informClockSource(ptpClock);
#endif /* __APPLE__ */
	}

/* Update relevant statistics containers, feed outlier filter thresholds etc. */
#ifdef PTPD_STATISTICS
statistics:
                        if (rtOpts->delayMSOutlierFilterEnabled) {
                        	double dDelayMS = timeInternalToDouble(&ptpClock->rawDelayMS);
                        	if(ptpClock->delayMSoutlier) {
				/* Allow [weight] * [deviation from mean] to influence std dev in the next outlier checks */
                        		DBG("DelayMS Outlier: %.09f\n", dDelayMS);
                        		dDelayMS = ptpClock->delayMSRawStats->meanContainer->mean + 
						    rtOpts->delayMSOutlierWeight * ( dDelayMS - ptpClock->delayMSRawStats->meanContainer->mean);
                        	}
                                feedDoubleMovingStdDev(ptpClock->delayMSRawStats, dDelayMS);
                                feedDoubleMovingMean(ptpClock->delayMSFiltered, timeInternalToDouble(&ptpClock->delayMS));
                        }
                        feedDoublePermanentStdDev(&ptpClock->slaveStats.ofmStats, timeInternalToDouble(&ptpClock->offsetFromMaster));
#ifdef PTPD_DOUBLE_SERVO
                        feedDoublePermanentStdDev(&ptpClock->servo.driftStats, ptpClock->servo.observedDrift);
#else
                        feedIntPermanentStdDev(&ptpClock->servo.driftStats, ptpClock->servo.observedDrift);
#endif /* PTPD_DOUBLE_SERVO */

#endif /* PTPD_STATISTICS */

display:
		logStatistics(rtOpts, ptpClock);

	DBGV("\n--Offset Correction-- \n");
	DBGV("Raw offset from master:  %10ds %11dns\n",
	    ptpClock->delayMS.seconds,
	    ptpClock->delayMS.nanoseconds);

	DBGV("\n--Offset and Delay filtered-- \n");

	if (ptpClock->delayMechanism == P2P) {
		DBGV("one-way delay averaged (P2P):  %10ds %11dns\n",
		    ptpClock->peerMeanPathDelay.seconds, 
		    ptpClock->peerMeanPathDelay.nanoseconds);
	} else if (ptpClock->delayMechanism == E2E) {
		DBGV("one-way delay averaged (E2E):  %10ds %11dns\n",
		    ptpClock->meanPathDelay.seconds, 
		    ptpClock->meanPathDelay.nanoseconds);
	}

	DBGV("offset from master:      %10ds %11dns\n",
	    ptpClock->offsetFromMaster.seconds, 
	    ptpClock->offsetFromMaster.nanoseconds);
	DBGV("observed drift:          %10d\n", ptpClock->servo.observedDrift);
}

void
setupPIservo(PIservo* servo, const RunTimeOpts* rtOpts)
{
    servo->maxOutput = rtOpts->servoMaxPpb;
    servo->kP = rtOpts->servoKP;
    servo->kI = rtOpts->servoKI;
#ifdef PTPD_STATISTICS
    servo->stabilityThreshold = rtOpts->servoStabilityThreshold;
    servo->stabilityPeriod = rtOpts->servoStabilityPeriod;
    servo->stabilityTimeout = (60 / rtOpts->statsUpdateInterval) * rtOpts->servoStabilityTimeout;
#endif
}

void
resetPIservo(PIservo* servo)
{
/* not needed: restoreDrift handles this */
/*   servo->observedDrift = 0; */
    servo->input = 0;
    servo->output = 0;
    servo->lastUpdate.seconds = 0;
    servo->lastUpdate.nanoseconds = 0;
}


#ifndef PTPD_DOUBLE_SERVO

Integer32
runPIservo(PIservo* servo, const Integer32 input)
{

	Integer32 aP;
	Integer32 aI;
#if 0
	double dt;
	TimeInternal now, delta;
	getTime(&now);
	if(servo->lastUpdate.seconds == 0 &&
	    servo->lastUpdate.nanoseconds == 0) {
	    dt = 1.0;
	} else {
	    subTime(&delta, &now, &servo->lastUpdate);
	    dt = delta.nanoseconds / 1E9;
	}

#endif

	servo->input = input;

	if (servo->kP < 1)
		servo->kP = 1;
	if (servo->kI < 1)
		servo->kI = 1;

	/*
	 * kP and kI are scaled to 10000
	 * because of integer overflow we convert them to attenuations
	 */

	aP = 10000 / servo->kP;
	aI = 10000 / servo->kI;
    
	if (aP <1)
		aP = 1;
	if (aI < 1)
		aI = 1;
    
    
#if 0
	/* 
	 * Inactive along with the dt calculation above:
	 * this allows for a reliable delta measurement,
	 * and decoupling kP and kI from sync interval,
	 * but sync becomes as slow as with 1 sync/sec.
	 * To be tested: employ this and work out new
	 * kP and kI defaults.
	 */
	servo->observedDrift +=
		dt * input / aI;
#else
	servo->observedDrift +=
		input / aI;
#endif
	if(servo->observedDrift >= servo->maxOutput) {
		servo->observedDrift = servo->maxOutput;
		servo->runningMaxOutput = TRUE;
	}
	else if(servo->observedDrift <= -servo->maxOutput) {
		servo->observedDrift = -servo->maxOutput;
		servo->runningMaxOutput = TRUE;
	} else {
		servo->runningMaxOutput = FALSE;
	}
	servo->output = input / aP + servo->observedDrift;

#if 0
	servo->lastUpdate = now;
	DBGV("Servo dt: %.09f, input (ofm): %d, output(adj): %d, accumulator (observed drift): %d\n", dt, input, servo->output, servo->observedDrift);
#else
	DBGV("Servo input (ofm): %d, output(adj): %d, accumulator (observed drift): %d\n", input, servo->output, servo->observedDrift);
#endif
	return -servo->output;

}

#else

double
runPIservo(PIservo* servo, const Integer32 input)
{

#if 0
	double dt;
	TimeInternal now, delta;
	getTime(&now);
	if(servo->lastUpdate.seconds == 0 &&
	    servo->lastUpdate.nanoseconds == 0) {
	    dt = 1.0;
	} else {
	    subTime(&delta, &now, &servo->lastUpdate);
	    dt = delta.nanoseconds / 1E9;
	}

#endif

	servo->input = input;

	if (servo->kP < 1)
		servo->kP = 1;
	if (servo->kI < 1)
		servo->kI = 1;

	/*
	 * kP and kI are scaled to 10000 - but we divide by 10000 first
	 * so that we don't lose precision
	 */

#if 0
	/* 
	 * Inactive along with the dt calculation above:
	 * this allows for a reliable delta measurement,
	 * and decoupling kP and kI from sync interval,
	 * but sync becomes as slow as with 1 sync/sec.
	 * To be tested: employ this and work out new
	 * kP and kI defaults.
	 */
	servo->observedDrift +=
		dt * ((input + 0.0) * servo->kI) / 10000;
#else
	servo->observedDrift +=
		((input + 0.0 ) * servo->kI) / 10000;
#endif
	if(servo->observedDrift >= servo->maxOutput) {
		servo->observedDrift = servo->maxOutput;
		servo->runningMaxOutput = TRUE;
	}
	else if(servo->observedDrift <= -servo->maxOutput) {
		servo->observedDrift = -servo->maxOutput;
		servo->runningMaxOutput = TRUE;
	} else {
		servo->runningMaxOutput = FALSE;
	}


	/*
	 * kP and kI are scaled to 10000 - but we divide by 10000 first
	 * so that we don't lose precision
	 */
	servo->output = (servo->kP * (input + 0.0) ) / 10000 + servo->observedDrift;

#if 0
	servo->lastUpdate = now;
	DBGV("Servo dt: %.09f, input (ofm): %d, output(adj): %.09f, accumulator (observed drift): %.09f\n", dt, input, servo->output, servo->observedDrift);
#else
	DBGV("Servo input (ofm): %d, output(adj): %d, accumulator (observed drift): %d\n", input, servo->output, servo->observedDrift);
#endif
	return -servo->output;

}



#endif /* PTPD_DOUBLE_SERVO */

#ifdef PTPD_STATISTICS
void
updatePtpEngineStats (PtpClock* ptpClock, RunTimeOpts* rtOpts)
{

                        DBG("Refreshing slave engine stats counters\n");
                        ptpClock->slaveStats.owdMean = ptpClock->slaveStats.owdStats.meanContainer.mean;
                        ptpClock->slaveStats.owdStdDev = ptpClock->slaveStats.owdStats.stdDev;
                        ptpClock->slaveStats.ofmMean = ptpClock->slaveStats.ofmStats.meanContainer.mean;
                        ptpClock->slaveStats.ofmStdDev = ptpClock->slaveStats.ofmStats.stdDev;
                        ptpClock->slaveStats.statsCalculated = TRUE;
                        ptpClock->servo.driftMean = ptpClock->servo.driftStats.meanContainer.mean;
                        ptpClock->servo.driftStdDev = ptpClock->servo.driftStats.stdDev;
                        ptpClock->servo.statsCalculated = TRUE;
			/* Handle the calibration delay - x periods of stats updates until servo starts */
			if(rtOpts->calibrationDelay) {
				++ptpClock->statsUpdates;
				if(!ptpClock->isCalibrated && ptpClock->statsUpdates >= rtOpts->calibrationDelay) {
				    NOTICE("PTP engine now calibrated - enabling clock control\n");
				    ptpClock->isCalibrated = TRUE;
				    ptpClock->statsUpdates = 0;
				}
			}
	if( (rtOpts->calibrationDelay == 0) || ptpClock->isCalibrated )
	if(rtOpts->servoStabilityDetection) {
                ++ptpClock->servo.updateCount;
                        if ( !ptpClock->servo.runningMaxOutput && (ptpClock->servo.driftStdDev <= ptpClock->servo.stabilityThreshold))
                            ++ptpClock->servo.stableCount;
                        else
                            ptpClock->servo.stableCount = 0;

                        /* Servo considered stable - drift std dev below threshold for n measurements - saving drift*/
                        if(ptpClock->servo.stableCount >= ptpClock->servo.stabilityPeriod) {
                                if(!ptpClock->servo.isStable) {
                                        NOTICE("Clock servo now stable - took %d seconds\n",
                                                rtOpts->statsUpdateInterval * ptpClock->servo.updateCount);
                                        saveDrift(ptpClock, rtOpts, FALSE);
                                } else {
                                       saveDrift(ptpClock, rtOpts, TRUE);
                                }
                                ptpClock->servo.isStable = TRUE;
                                ptpClock->servo.stableCount = 0;
                                ptpClock->servo.updateCount = 0;
                        } else if(ptpClock->servo.updateCount >= ptpClock->servo.stabilityTimeout) {
                                ptpClock->servo.stableCount = 0;
                                ptpClock->servo.updateCount = 0;
                                if(!ptpClock->servo.isStable) {
                                        if(ptpClock->servo.runningMaxOutput) {
                                        WARNING("Clock servo not stable after %d seconds - running at maximum rate.\n", rtOpts->statsUpdateInterval * ptpClock->servo.stabilityTimeout);
                                        } else {
                                        WARNING("Clock servo not stable after %d seconds. Saving current observed drift.\n", rtOpts->statsUpdateInterval * ptpClock->servo.stabilityTimeout);
                                        saveDrift(ptpClock, rtOpts, FALSE);
                                        }
                                } else {
                                        WARNING("Clock servo no longer stable\n");
                                        ptpClock->servo.isStable = FALSE;
                                }
                        }
			}

                        DBG("servo stablecount: %d\n",ptpClock->servo.stableCount);

                        resetDoublePermanentStdDev(&ptpClock->slaveStats.owdStats);
                        resetDoublePermanentStdDev(&ptpClock->slaveStats.ofmStats);
#ifdef PTPD_DOUBLE_SERVO
                        resetDoublePermanentStdDev(&ptpClock->servo.driftStats);
#else
                        resetIntPermanentStdDev(&ptpClock->servo.driftStats);
#endif /* PTPD_DOUBLE_SERVO */

}

#endif /* PTPD_STATISTICS */
