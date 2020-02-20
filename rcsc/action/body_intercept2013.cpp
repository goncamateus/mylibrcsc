// -*-c++-*-

/*!
  \file body_intercept2013.cpp
*/

/*
 *Copyright:

 Copyright (C) Hidehisa AKIYAMA

 This code is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 3 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 *EndCopyright:
 */

/////////////////////////////////////////////////////////////////////

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "body_intercept2013.h"

#include <rcsc/action/basic_actions.h>
#include <rcsc/action/body_go_to_point.h>

#include <rcsc/player/intercept_table.h>
#include <rcsc/player/player_agent.h>
#include <rcsc/player/debug_client.h>
#include <rcsc/common/logger.h>
#include <rcsc/common/server_param.h>
#include <rcsc/soccer_math.h>
#include <rcsc/math_util.h>

#define DEBUG_PRINT

#define USE_GOALIE_MODE

namespace rcsc {

// namespace {
// char
// type_char( const InterceptInfo::ActionType t )
// {
//     switch ( t ) {
//     case InterceptInfo::OMNI_DASH:
//         return 'o';
//     case InterceptInfo::TURN_FORWARD_DASH:
//         return 'f';
//     case InterceptInfo::TURN_BACK_DASH:
//         return 'b';
//     default:
//         break;
//     }
//     return 'u';
// }
// }

/*-------------------------------------------------------------------*/
/*!

*/
bool
Body_Intercept2013::execute( PlayerAgent * agent )
{
    dlog.addText( Logger::TEAM,
                  __FILE__": Body_Intercept2013" );

    const WorldModel & wm = agent->world();
    const InterceptTable * table = wm.interceptTable();

    if ( table->selfReachCycle() > 100 )
    {
        doGoToFinalPoint( agent );
        return true;
    }

    InterceptInfo best_intercept = get_best_intercept( wm, M_save_recovery );

    // debug output
    {
        Vector2D ball_pos = wm.ball().inertiaPoint( best_intercept.reachCycle() );
        agent->debugClient().setTarget( ball_pos );

        dlog.addText( Logger::INTERCEPT,
                      __FILE__": solution size=%d. best_cycle=%d"
                      "(turn:%d dash:%d) (dash power=%.1f dir=%.1f)",
                      table->selfCache().size(),
                      best_intercept.reachCycle(),
                      best_intercept.turnCycle(), best_intercept.dashCycle(),
                      best_intercept.dashPower(), best_intercept.dashAngle() );
        dlog.addText( Logger::INTERCEPT,
                      __FILE__": ball pos=(%.2f %.2f)",
                      ball_pos.x, ball_pos.y );
    }

    if ( doOneStepTurn( agent, best_intercept ) )
    {
        return true;
    }

    if ( doFirstTurn( agent, best_intercept ) )
    {
        return true;
    }

    if ( doSaveRecovery( agent, best_intercept ) )
    {
        return true;
    }

    doDash( agent, best_intercept );
    return true;
}

/*-------------------------------------------------------------------*/
/*!

*/
bool
Body_Intercept2013::doGoToFinalPoint( PlayerAgent * agent )
{
    const WorldModel & wm = agent->world();

    Vector2D final_point = wm.ball().inertiaFinalPoint();
    agent->debugClient().setTarget( final_point );

    dlog.addText( Logger::INTERCEPT,
                  __FILE__": (doGoToFinalPoint) no solution... go to (%.2f %.2f)",
                  final_point.x, final_point.y );
    agent->debugClient().addMessage( "InterceptNoSolution" );

    Body_GoToPoint( final_point,
                    wm.self().playerType().kickableArea() - 0.2,
                    ServerParam::i().maxDashPower() ).execute( agent );

    return true;
}

/*-------------------------------------------------------------------*/
/*!

*/
bool
Body_Intercept2013::doOneStepTurn( PlayerAgent * agent,
                                   const InterceptInfo & info )
{
    if ( info.reachCycle() > 1
         || info.dashCycle() > 0 )
    {
        return false;
    }

    const WorldModel & wm = agent->world();

    Vector2D face_point = M_face_point;
    if ( ! face_point.isValid() )
    {
        face_point.assign( 44.0, wm.self().pos().y * 0.75 );
    }

    dlog.addText( Logger::INTERCEPT,
                  __FILE__": (doOneStepTurn) true: face point=(%.2f %.2f)",
                  face_point.x, face_point.y );
    agent->debugClient().addMessage( "InterceptTurnOnly" );

    Body_TurnToPoint( face_point, 100 ).execute( agent );

    return true;
}

/*-------------------------------------------------------------------*/
/*!

*/
bool
Body_Intercept2013::doFirstTurn( PlayerAgent * agent,
                                 const InterceptInfo & info )
{
    if ( info.turnCycle() == 0 )
    {
        return false;
    }

    const WorldModel & wm = agent->world();

    const Vector2D self_pos = wm.self().inertiaPoint( info.reachCycle() );
    const Vector2D ball_pos = wm.ball().inertiaPoint( info.reachCycle() );

    AngleDeg ball_angle = ( ball_pos - self_pos ).th();
    if ( info.dashPower() < 0.0 )
    {
        ball_angle -= 180.0;
    }

    dlog.addText( Logger::INTERCEPT,
                  __FILE__": (doFirstTurn) [%s] target_body_angle = %.1f",
                  ( info.dashPower() < 0.0 ? "back" : "forward" ),
                  ball_angle.degree() );
    agent->debugClient().addMessage( "InterceptTurn%d(%d:%d)",
                                     info.reachCycle(), info.turnCycle(), info.dashCycle() );

    agent->doTurn( ball_angle - wm.self().body() );
    return true;
}

/*-------------------------------------------------------------------*/
/*!

*/
bool
Body_Intercept2013::doSaveRecovery( PlayerAgent * agent,
                                    const InterceptInfo & info )
{
    const WorldModel & wm = agent->world();

    if ( M_save_recovery
         && ! wm.self().staminaModel().capacityIsEmpty() )
    {
        double consumed_stamina = info.dashPower();
        if ( info.dashPower() < 0.0 ) consumed_stamina *= -2.0;

        if ( wm.self().stamina() - consumed_stamina < ServerParam::i().recoverDecThrValue() + 1.0 )
        {
            dlog.addText( Logger::INTERCEPT,
                          __FILE__": (doSaveRecovery) insufficient stamina" );
            agent->debugClient().addMessage( "InterceptRecover" );
            agent->doTurn( 0.0 );
            return true;
        }
    }

    return false;
}

/*-------------------------------------------------------------------*/
/*!

*/
bool
Body_Intercept2013::doDashWait( PlayerAgent * agent,
                                const InterceptInfo & info )
{
    const WorldModel & wm = agent->world();

    const Vector2D self_pos = wm.self().inertiaPoint( info.reachCycle() );
    const Vector2D ball_pos = wm.ball().inertiaPoint( info.reachCycle() );
    const bool goalie_mode = ( wm.self().goalie()
                               && wm.lastKickerSide() != wm.ourSide()
                               && ball_pos.x < ServerParam::i().ourPenaltyAreaLineX()
                               && ball_pos.absY() < ServerParam::i().penaltyAreaHalfWidth() );
    const double control_area = ( goalie_mode
                                  ? wm.self().playerType().reliableCatchableDist()
                                  : wm.self().playerType().kickableArea() );

    if ( self_pos.dist2( ball_pos ) > std::pow( control_area - 0.3, 2 ) )
    {
        dlog.addText( Logger::INTERCEPT,
                      __FILE__": (doDashWait) still need dash" );

        return false;
    }


    Vector2D face_point = M_face_point;
    if ( ! M_face_point.isValid() )
    {
        face_point.assign( 50.5, wm.self().pos().y * 0.75 );
    }

    if ( info.reachCycle() >= 2
         && wm.ball().seenPosCount() > 3 )
    {
        face_point = wm.ball().pos() + wm.ball().vel();
        dlog.addText( Logger::INTERCEPT,
                      __FILE__": (doDashWait) check ball. pos_count=%d",
                      wm.ball().seenPosCount() );
    }
    else if ( info.reachCycle() >= 4 )
    {
        Vector2D my_inertia = wm.self().inertiaFinalPoint();
        AngleDeg face_angle = ( face_point - my_inertia ).th();
        Vector2D next_ball_pos = wm.ball().pos() + wm.ball().vel();
        AngleDeg next_ball_angle = ( next_ball_pos - my_inertia ).th();
        double normal_half_width = ViewWidth::width( ViewWidth::NORMAL );

        if ( ( next_ball_angle - face_angle ).abs()
             > ( ServerParam::i().maxNeckAngle() + normal_half_width - 10.0 ) )
        {
            face_point.x = my_inertia.x;
            if ( next_ball_pos.y > my_inertia.y + 1.0 )
            {
                face_point.y = 50.0;
                dlog.addText( Logger::INTERCEPT,
                              __FILE__": (doDashWait) check ball with turn. adjust(1)" );

            }
            else if ( next_ball_pos.y < my_inertia.y - 1.0 )
            {
                face_point.y = -50.0;
                dlog.addText( Logger::INTERCEPT,
                              __FILE__": (doDashWait) check ball with turn. adjust(2)" );
            }
            else
            {
                face_point = next_ball_pos;
                dlog.addText( Logger::INTERCEPT,
                              __FILE__": (doDashWait) check ball with turn. adjust(3)" );
            }
        }
        else
        {
            dlog.addText( Logger::INTERCEPT,
                          __FILE__": (doDashWait) check ball without turn" );
        }
    }

    agent->debugClient().addMessage( "InterceptDashWait%d", info.reachCycle() );
    dlog.addText( Logger::INTERCEPT,
                  __FILE__": (doDashWait) turn to (%.2f %.2f)" );

    Body_TurnToPoint( face_point, 100 ).execute( agent );
    return true;
}

/*-------------------------------------------------------------------*/
/*!

*/
bool
Body_Intercept2013::doOneAdjustDash( PlayerAgent * agent,
                                     const InterceptInfo & info )
{
    const WorldModel & wm = agent->world();
    const PlayerType & ptype = wm.self().playerType();

    Vector2D self_next = wm.self().pos() + wm.self().vel();
    Vector2D ball_next = wm.ball().pos() + wm.ball().vel();

    double ball_dist = self_next.dist( ball_next );
    if ( ball_dist < ptype.kickableArea() - 0.3 )
    {
        double ball_speed = wm.ball().vel().r() * ServerParam::i().ballDecay();
        double kick_rate = ptype.kickRate( ball_dist, 180.0 );

        if ( ( ball_next.x > ServerParam::i().pitchHalfLength() - 4.0
               && ball_next.absY() < ServerParam::i().goalHalfWidth() + 1.0
               && wm.ball().vel().x > 0.0 )
             || ServerParam::i().maxPower() * kick_rate + 0.3 > ball_speed )
        {
            if ( doDashWait( agent, info ) )
            {
                return true;
            }
        }
    }

    agent->debugClient().addMessage( "Intercept1Dash%.0f:%.0f",
                                     info.dashPower(), info.dashAngle() );
    dlog.addText( Logger::INTERCEPT,
                  __FILE__": (doOneAdjustDash) 1 dash" );
    agent->doDash( info.dashPower(), info.dashAngle() );
    return true;
}

/*-------------------------------------------------------------------*/
/*!

*/
bool
Body_Intercept2013::doDash( PlayerAgent * agent,
                            const InterceptInfo & info )
{
    if ( info.reachCycle() == 1 )
    {
        doOneAdjustDash( agent, info );
        //agent->doDash( info.dashPower(), info.dashAngle() );
        return true;
    }

    //
    //
    //
    if ( doDashWait( agent, info ) )
    {
        return true;
    }

    //
    //
    //
    agent->debugClient().addMessage( "InterceptDash%d:%.0f:%.0f",
                                     info.reachCycle(),
                                     info.dashPower(), info.dashAngle() );
    dlog.addText( Logger::INTERCEPT,
                  __FILE__": (doDash) power=%.3f dir=%.1f",
                  info.dashPower(), info.dashAngle() );

    agent->doDash( info.dashPower(), info.dashAngle() );
    return true;
}

/*-------------------------------------------------------------------*/
/*!

*/
InterceptInfo
Body_Intercept2013::get_best_intercept( const WorldModel & wm,
                                        const bool save_recovery )
{
    if ( wm.self().goalie() )
    {
        return get_best_intercept_goalie( wm );
    }
    return get_best_intercept2013( wm, save_recovery );
}

namespace {
/*-------------------------------------------------------------------*/
/*!

*/
struct InterceptCandidate {
    const InterceptInfo * info_;
    double value_;

    explicit
    InterceptCandidate( const InterceptInfo * info,
                        const double value )
        : info_( info ),
          value_( value )
      { }
};

/*-------------------------------------------------------------------*/
/*!

*/
struct InterceptCandidateSorter {
    bool operator()( const InterceptCandidate & lhs,
                     const InterceptCandidate & rhs ) const
      {
          return lhs.value_ > rhs.value_;
      }
};
}

/*-------------------------------------------------------------------*/
/*!

*/
InterceptInfo
Body_Intercept2013::get_best_intercept2013( const WorldModel & wm,
                                            const bool save_recovery )
{
    static GameTime s_time;
    static InterceptInfo s_cached_best;

    if ( s_time == wm.time() )
    {
        return s_cached_best;
    }
    s_time = wm.time();

    const ServerParam & SP = ServerParam::i();

    const double max_x = ( SP.keepawayMode()
                           ? SP.keepawayLength() * 0.5 - 1.0
                           : SP.pitchHalfLength() - 1.0 );
    const double max_y = ( SP.keepawayMode()
                           ? SP.keepawayWidth() * 0.5 - 1.0
                           : SP.pitchHalfWidth() - 1.0 );
    const double kickable_area = wm.self().playerType().kickableArea();
    const double fast_speed_thr = wm.self().playerType().realSpeedMax() * 0.8;
    const double slow_ball_speed_thr = 0.55;
    const double first_ball_speed = wm.ball().vel().r();
    const AngleDeg ball_vel_angle = wm.ball().vel().th();

    const int opponent_step = wm.interceptTable()->opponentReachCycle();
    const int teammate_step = wm.interceptTable()->teammateReachCycle();

    std::vector< InterceptCandidate > candidates;
    candidates.reserve( wm.interceptTable()->selfCache().size() );

    int count = 1;
    for ( std::vector< InterceptInfo >::const_iterator it = wm.interceptTable()->selfCache().begin(),
              end = wm.interceptTable()->selfCache().end();
          it != end;
          ++it, ++count )
    {
        if ( save_recovery
             && it->mode() != InterceptInfo::NORMAL )
        {
            continue;
        }

        const Vector2D ball_pos = wm.ball().inertiaPoint( it->reachCycle() );
        double turn_angle = (ball_pos.th() - wm.self().body()).abs();
        const AngleDeg body_angle = wm.self().body() + turn_angle;
        const double ball_speed = first_ball_speed * std::pow( SP.ballDecay(), it->reachCycle() );
        candidates.push_back( InterceptCandidate( &(*it), 0.0 ) );
        InterceptCandidate & candidate = candidates.back();

        //
        // base value
        //
        // candidate.value_ = -it->reachCycle();

        // dlog.addText( Logger::INTERCEPT,
        //               "%d: (intercept eval) base value = %.3f",
        //               count, candidate.value_ );

        if ( ball_pos.absX() > max_x
             || ball_pos.absY() > max_y )
        {
            candidate.value_ = -1000.0 - it->reachCycle();
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) out of pitch = %.3f",
                          count, candidate.value_ );
#endif
            continue;
        }

        double tmp_val;

        //
        //
        //
        {
            double spot_x_dist = std::fabs( ball_pos.x - 44.0 );
            double spot_y_dist = ball_pos.absY() * 0.5;
            double spot_dist = std::sqrt( std::pow( spot_x_dist, 2 ) + std::pow( spot_y_dist, 2 ) );
            tmp_val = spot_dist * -0.2;
            candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) shoot spot dist = %.3f (%.3f)",
                          count, tmp_val, candidate.value_ );
#endif
        }

        //
        //
        //
//         if ( it->reachCycle() == 1
//              && it->ballDist() < kickable_area - 0.3 )
//         {
//             tmp_val = 0.5;
//             candidate.value_ += tmp_val;
// #ifdef DEBUG_PRINT
//             dlog.addText( Logger::INTERCEPT,
//                           "%d: (intercept eval) 1 step = %.3f (%.3f)",
//                           count, tmp_val, candidate.value_ );
// #endif
//         }

        //
        //
        //
        tmp_val = 0.0;
        if ( wm.gameMode().type() == GameMode::GoalKick_
             && wm.gameMode().side() == wm.ourSide()
             && ball_pos.x < SP.ourPenaltyAreaLineX() - 2.0
             && ball_pos.absY() < SP.penaltyAreaHalfWidth() - 2.0 )
        {
            // no penalty
        }
        else
        {
            if ( opponent_step <= it->reachCycle() + 3 )
            {
                tmp_val = ( opponent_step - ( it->reachCycle() + 3 ) ) * 5.0;
            }
        }
        candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
        dlog.addText( Logger::INTERCEPT,
                      "%d: (intercept eval) opponent diff = %.3f (%.3f)",
                      count, tmp_val, candidate.value_ );
#endif
        //
        //
        //
        tmp_val = 0.0;
        if ( teammate_step <= it->reachCycle() + 3 )
        {
            tmp_val = ( teammate_step - ( it->reachCycle() + 3 ) ) * 0.5;
        }
        candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
        dlog.addText( Logger::INTERCEPT,
                      "%d: (intercept eval) teammate diff = %.3f (%.3f)",
                      count, tmp_val, candidate.value_ );
#endif
        //
        //
        //
        // if ( it->actionType() == InterceptInfo::TURN_FORWARD_DASH
        //      || it->actionType() == InterceptInfo::TURN_BACK_DASH )
        // {
        if ( it->ballDist() > kickable_area - 0.3
                && it->turnCycle() > 0 )
        {
            tmp_val = std::fabs( turn_angle ) * -0.025;
            candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                            "%d: (intercept eval) turn penalty = %.3f (angle=%.1f) (%.3f)",
                            count, tmp_val, turn_angle, candidate.value_ );
#endif
        }
        // }

        {
            //
            //
            //
            double move_dist = it->selfPos().dist( wm.self().pos() );
            tmp_val = 0.0;
            if ( ball_pos.x < wm.offsideLineX() )
            {
                tmp_val = move_dist * -0.3; //-0.1;
            }
            if ( it->ballDist() > kickable_area - 0.3 )
            {
                tmp_val += move_dist * -0.5; //-0.2;
            }
            candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) move dist penalty = %.3f (%.3f)",
                          count, tmp_val, candidate.value_ );
#endif
        }

        //
        //
        //
        if ( it->ballDist() > kickable_area - 0.4 )
        {
            //tmp_val = ( it->ballDist() - ( kickable_area - 0.4 ) ) * -5.0;
            tmp_val = ( it->ballDist() - ( kickable_area - 0.4 ) ) * -3.0 - 0.5;
            candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                              "%d: (intercept eval) ball dist penalty(1) = %.3f (%.3f)",
                          count, tmp_val, candidate.value_ );
#endif
        }

        //
        //
        //
        {
            tmp_val = ( it->stamina() - wm.self().stamina() ) * 0.0001;
            candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) stamina penalty = %.3f (%.3f)",
                          count, tmp_val, candidate.value_ );
#endif
        }

        //
        //
        //
        if ( it->turnCycle() > 0 )
        {
            tmp_val = -0.01;
            candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) turn penalty = %.3f (%.3f)",
                          count, tmp_val, candidate.value_ );
#endif
        }

        if ( it->turnCycle() > 0
             && it->dashPower() < 0.0 )
        {
            tmp_val = std::max( 0.0, body_angle.abs() - 90.0 ) * -0.1;
            candidate.value_ += tmp_val;
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) turn back penalty = %.3f (%.3f)",
                          count, tmp_val, candidate.value_ );

            if ( ( wm.self().body() - body_angle ).abs() > 90.0 )
            {
                tmp_val = -0.001;
                candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
                dlog.addText( Logger::INTERCEPT,
                              "%d: (intercept eval) turn back penalty(2) = %.3f (%.3f)",
                              count, tmp_val, candidate.value_ );
#endif
            }
        }

        //
        //
        //
        if ( ( body_angle - ball_vel_angle ).abs() < 30.0 //aligned_body_angle_to_ball_move_line
             && Segment2D( wm.ball().pos(), ball_pos ).dist( wm.self().pos() ) < kickable_area - 0.3 )
        {
            if ( ball_speed < fast_speed_thr )
            {
                tmp_val = ( fast_speed_thr - ball_speed ) * -20.0;
                candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
                dlog.addText( Logger::INTERCEPT,
                              "%d: (intercept eval) ball speed penalty(1) = %.3f (%.3f)",
                              count, tmp_val, candidate.value_ );
#endif
            }
        }
        else
        {
            if ( ball_speed < slow_ball_speed_thr ) // magic number
            {
                //tmp_val = ( slow_ball_speed_thr - ball_speed ) * -20.0;
                tmp_val = std::pow( slow_ball_speed_thr - ball_speed, 2 ) * -70.0;
                candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
                dlog.addText( Logger::INTERCEPT,
                              "%d: (intercept eval) ball speed penalty(2) = %.3f (%.3f)",
                              count, tmp_val, candidate.value_ );
#endif
            }
        }
    }
    std::vector< InterceptCandidate >::const_iterator best = std::min_element( candidates.begin(),
                                                                               candidates.end(),
                                                                               InterceptCandidateSorter() );
    if ( best != candidates.end() )
    {
        s_cached_best = *(best->info_);
        return s_cached_best;
    }

    if ( candidates.empty() )
    {
        s_cached_best = InterceptInfo();
    }
    else
    {
        s_cached_best = *(candidates.front().info_);
    }

    return s_cached_best;
}


/*-------------------------------------------------------------------*/
/*!

*/
InterceptInfo
Body_Intercept2013::get_best_intercept_goalie( const WorldModel & wm )
{
    static GameTime s_time;
    static InterceptInfo s_cached_best;

    if ( s_time == wm.time() )
    {
        return s_cached_best;
    }
    s_time = wm.time();

    const ServerParam & SP = ServerParam::i();

    const double max_x = SP.pitchHalfLength();
    const double max_y = SP.pitchHalfWidth();

    const double kickable_area = wm.self().playerType().kickableArea();
    const double catchable_area = wm.self().playerType().reliableCatchableDist();

    const int opponent_step = wm.interceptTable()->opponentReachCycle();

    std::vector< InterceptCandidate > candidates;
    candidates.reserve( wm.interceptTable()->selfCache().size() );

    int count = 0;
    for ( std::vector< InterceptInfo >::const_iterator it = wm.interceptTable()->selfCache().begin(),
              end = wm.interceptTable()->selfCache().end();
          it != end;
          ++it, ++count )
    {
        const Vector2D ball_pos = wm.ball().inertiaPoint( it->reachCycle() );
        double turn_angle = (ball_pos.th() - wm.self().body()).abs();
        const AngleDeg body_angle = wm.self().body() + turn_angle;
        const double control_area = ( ball_pos.x < SP.ourPenaltyAreaLineX() - 0.5
                                      && ball_pos.absY() < SP.penaltyAreaHalfWidth() - 0.5
                                      ? catchable_area
                                      : kickable_area );

        candidates.push_back( InterceptCandidate( &(*it), 0.0 ) );
        InterceptCandidate & candidate = candidates.back();

        if ( it->mode() != InterceptInfo::NORMAL )
        {
            candidate.value_ -= 1000.0;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) recovery decay = %.3f",
                          count, candidate.value_ );
#endif
        }

        if ( ball_pos.absX() > max_x
             || ball_pos.absY() > max_y )
        {
            candidate.value_ -= ( 1000.0 + it->reachCycle() );
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) out of pitch = %.3f",
                          count, candidate.value_ );
#endif
            continue;
        }

        double tmp_val;

        {
            tmp_val = ball_pos.x;
            candidate.value_ = tmp_val;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) ball.x = %.3f (%.3f)",
                          count, tmp_val, candidate.value_ );
#endif
        }

        if ( opponent_step <= it->reachCycle() + 3 )
        {
            tmp_val = ( opponent_step - ( it->reachCycle() + 3 ) ) * 5.0;
            candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) opponent diff = %.3f (%.3f)",
                          count, tmp_val, candidate.value_ );
#endif
        }

        // if ( it->actionType() == InterceptInfo::TURN_FORWARD_DASH
        //      || it->actionType() == InterceptInfo::TURN_BACK_DASH )
        // {
            if ( it->ballDist() > control_area - 0.3
                 && it->turnCycle() > 0 )
            {
                tmp_val = std::fabs( turn_angle ) * -0.025;
                candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
                dlog.addText( Logger::INTERCEPT,
                              "%d: (intercept eval) turn penalty = %.3f (moment=%.1f) (%.3f)",
                              count, tmp_val, turn_angle, candidate.value_ );
#endif
            }
        // }

        {
            tmp_val = -1.0 * it->ballDist();
            tmp_val = ( it->ballDist() - ( kickable_area - 0.4 ) ) * -3.0 - 0.5;
            candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) ball dist penalty = %.3f (%.3f)",
                          count, tmp_val, candidate.value_ );
#endif
        }

        {
            tmp_val = -0.1 * body_angle.abs();
            candidate.value_ += tmp_val;
#ifdef DEBUG_PRINT
            dlog.addText( Logger::INTERCEPT,
                          "%d: (intercept eval) body angle penalty = %.3f (%.3f)",
                          count, tmp_val, candidate.value_ );
#endif
        }

    }


    std::vector< InterceptCandidate >::const_iterator best = std::min_element( candidates.begin(),
                                                                               candidates.end(),
                                                                               InterceptCandidateSorter() );
    if ( best != candidates.end() )
    {
        s_cached_best = *(best->info_);
        return s_cached_best;
    }

    if ( candidates.empty() )
    {
        s_cached_best = InterceptInfo();
    }
    else
    {
        s_cached_best = *(candidates.front().info_);
    }

    return s_cached_best;
}

}
