//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#include "cbase.h"
#include "usercmd.h"
#include "igamesystem.h"
#include "ilagcompensationmanager.h"
#include "inetchannelinfo.h"
#include "utllinkedlist.h"
#include "BaseAnimatingOverlay.h"
#ifdef NEXT_BOT
#include "NextBotInterface.h"
#endif //NEXT_BOT
#include "tier0/vprof.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define LC_NONE				0
#define LC_ALIVE			(1<<0)

#define LC_ORIGIN_CHANGED	(1<<8)
#define LC_ANGLES_CHANGED	(1<<9)
#define LC_SIZE_CHANGED		(1<<10)
#define LC_ANIMATION_CHANGED (1<<11)

static ConVar sv_lagcompensation_teleport_dist( "sv_lagcompensation_teleport_dist", "64", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT, "How far a player got moved by game code before we can't lag compensate their position back" );
#define LAG_COMPENSATION_EPS_SQR ( 0.1f * 0.1f )
// Allow 4 units of error ( about 1 / 8 bbox width )
#define LAG_COMPENSATION_ERROR_EPS_SQR ( 4.0f * 4.0f )

ConVar sv_unlag( "sv_unlag", "1", FCVAR_DEVELOPMENTONLY, "Enables player lag compensation" );
ConVar sv_maxunlag( "sv_maxunlag", "1.0", FCVAR_DEVELOPMENTONLY, "Maximum lag compensation in seconds", true, 0.0f, true, 1.0f );
ConVar sv_lagflushbonecache( "sv_lagflushbonecache", "1", FCVAR_DEVELOPMENTONLY, "Flushes entity bone cache on lag compensation" );
ConVar sv_showlagcompensation( "sv_showlagcompensation", "0", FCVAR_CHEAT, "Show lag compensated hitboxes whenever a player is lag compensated." );

ConVar sv_unlag_fixstuck( "sv_unlag_fixstuck", "0", FCVAR_DEVELOPMENTONLY, "Disallow backtracking a player for lag compensation if it will cause them to become stuck" );

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
#define MAX_LAYER_RECORDS (CBaseAnimatingOverlay::MAX_OVERLAYS)

struct LayerRecord
{
	int m_sequence;
	float m_cycle;
	float m_weight;
	int m_order;

	LayerRecord()
	{
		m_sequence = 0;
		m_cycle = 0;
		m_weight = 0;
		m_order = 0;
	}

	LayerRecord( const LayerRecord& src )
	{
		m_sequence = src.m_sequence;
		m_cycle = src.m_cycle;
		m_weight = src.m_weight;
		m_order = src.m_order;
	}
};

struct LagRecord
{
public:
	LagRecord()
	{
		m_fFlags = 0;
		m_vecOrigin.Init();
		m_vecAngles.Init();
		m_vecMinsPreScaled.Init();
		m_vecMaxsPreScaled.Init();
		m_flSimulationTime = -1;
		m_masterSequence = 0;
		m_masterCycle = 0;

		for( int i=0; i<MAXSTUDIOPOSEPARAM; i++ )
		{
			m_flPoseParameters[i] = 0;
		}
	}

	LagRecord( const LagRecord& src )
	{
		m_fFlags = src.m_fFlags;
		m_vecOrigin = src.m_vecOrigin;
		m_vecAngles = src.m_vecAngles;
		m_vecMinsPreScaled = src.m_vecMinsPreScaled;
		m_vecMaxsPreScaled = src.m_vecMaxsPreScaled;
		m_flSimulationTime = src.m_flSimulationTime;
		for( int layerIndex = 0; layerIndex < MAX_LAYER_RECORDS; ++layerIndex )
		{
			m_layerRecords[layerIndex] = src.m_layerRecords[layerIndex];
		}
		m_masterSequence = src.m_masterSequence;
		m_masterCycle = src.m_masterCycle;

		for( int i=0; i<MAXSTUDIOPOSEPARAM; i++ )
		{
			m_flPoseParameters[i] = src.m_flPoseParameters[i];
		}
	}

	// Did player die this frame
	int						m_fFlags;

	// Player position, orientation and bbox
	Vector					m_vecOrigin;
	QAngle					m_vecAngles;
	Vector					m_vecMinsPreScaled;
	Vector					m_vecMaxsPreScaled;

	float					m_flSimulationTime;	
	
	// Player animation details, so we can get the legs in the right spot.
	LayerRecord				m_layerRecords[MAX_LAYER_RECORDS];
	int						m_masterSequence;
	float					m_masterCycle;

	float					m_flPoseParameters[MAXSTUDIOPOSEPARAM];
};

static void LC_TraceEntity( CBaseEntity *pEntity, const Vector &vecStart, const Vector &vecEnd, const IHandleEntity *pIgnore, trace_t *ptr )
{
	Vector vecHullMins, vecHullMaxs;
	Vector vecStepOffset = vec3_origin; // Nextbots need to start higher for rough terrain
#ifdef NEXT_BOT
	if ( !pEntity->IsPlayer() && pEntity->IsNextBot() )
	{
		INextBot *pNextBot = pEntity->MyNextBotPointer();
		vecHullMins = pNextBot->GetBodyInterface()->GetHullMins();
		vecHullMaxs = pNextBot->GetBodyInterface()->GetHullMaxs();
		vecStepOffset = Vector( 0.f, 0.f, pNextBot->GetLocomotionInterface()->GetStepHeight() );
	}
	else
#endif //NEXT_BOT
	{
		vecHullMins = pEntity->CollisionProp()->OBBMins();
		vecHullMaxs = pEntity->CollisionProp()->OBBMaxs();
	}
	UTIL_TraceHull( vecStart + vecStepOffset, vecEnd, vecHullMins, vecHullMaxs, pEntity->PhysicsSolidMaskForEntity(), pIgnore, COLLISION_GROUP_PLAYER_MOVEMENT, ptr );
}

//
// Try to take the player from his current origin to vWantedPos.
// If it can't get there, leave the player where he is.
// 

ConVar sv_unlag_debug( "sv_unlag_debug", "0", FCVAR_GAMEDLL | FCVAR_DEVELOPMENTONLY );

float g_flFractionScale = 0.95;
static void RestoreEntityTo( CBaseEntity *pEntity, const Vector &vWantedPos )
{
	// Try to move to the wanted position from our current position.
	trace_t tr;
	VPROF_BUDGET( "RestoreEntityTo", "CLagCompensationManager" );
	LC_TraceEntity( pEntity, vWantedPos, vWantedPos, pEntity, &tr );
	if ( tr.startsolid || tr.allsolid )
	{
		if ( sv_unlag_debug.GetBool() )
		{
			DevMsg( "RestoreEntityTo could not restore player position for client \"%i\" ( %.1f %.1f %.1f )\n",
					pEntity->entindex(), vWantedPos.x, vWantedPos.y, vWantedPos.z);
		}

		LC_TraceEntity( pEntity, pEntity->GetLocalOrigin(), vWantedPos, pEntity, &tr );
		if ( tr.startsolid || tr.allsolid )
		{
			// In this case, the guy got stuck back wherever we lag compensated him to. Nasty.

			if ( sv_unlag_debug.GetBool() )
				DevMsg( " restore failed entirely\n" );
		}
		else
		{
			// We can get to a valid place, but not all the way back to where we were.
			Vector vPos;
			VectorLerp( pEntity->GetLocalOrigin(), vWantedPos, tr.fraction * g_flFractionScale, vPos );
			UTIL_SetOrigin( pEntity, vPos, true );

			if ( sv_unlag_debug.GetBool() )
				DevMsg( " restore got most of the way\n" );
		}
	}
	else
	{
		// Cool, the player can go back to whence he came.
		UTIL_SetOrigin( pEntity, tr.endpos, true );
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
class CLagCompensationManager : public CAutoGameSystemPerFrame, public ILagCompensationManager
{
public:
	CLagCompensationManager( char const *name )
		: CAutoGameSystemPerFrame( name )
		, m_flTeleportDistanceSqr( 64 * 64 )
		, m_mapCompensatedEntities( DefLessFunc( EHANDLE ) )
		, m_rbAdditionalEntities( DefLessFunc( EHANDLE ) )
	{
		m_isCurrentlyDoingCompensation = false;
	}

	// IServerSystem stuff
	virtual void Shutdown()
	{
		ClearHistory();
	}

	virtual void LevelShutdownPostEntity()
	{
		ClearHistory();
	}

	// called after entities think
	virtual void FrameUpdatePostEntityThink();

	// ILagCompensationManager stuff

	// Called during player movement to set up/restore after lag compensation
	void			StartLagCompensation( CBasePlayer *player, CUserCmd *cmd );
	void			FinishLagCompensation( CBasePlayer *player );

	bool			IsCurrentlyDoingLagCompensation() const OVERRIDE { return m_isCurrentlyDoingCompensation; }

	// Mappers can flag certain additional entities to lag compensate, this handles them
	virtual void	AddAdditionalEntity( EHANDLE hEntity );
	virtual void	RemoveAdditionalEntity( EHANDLE hEntity );

private:
	struct entitylagdata_t
	{
		bool								bRestoreEntity;	// did lag compensation alter entity data
		CUtlFixedLinkedList< LagRecord >	RecordTrack;	// this entity's list of lag records
		LagRecord							RestoreData;	// entity data before we moved him back
		LagRecord							ChangeData;		// entity data where we moved him back
	};

	void BacktrackEntity( CBaseEntity *pEntity, entitylagdata_t *pLagData, float flTargetTime );

	void ClearHistory()
	{
		m_mapCompensatedEntities.PurgeAndDeleteElements();
	}

	CUtlMap< EHANDLE, entitylagdata_t * > m_mapCompensatedEntities;

	bool					m_bNeedToRestore;	// Did any entity change
	CBasePlayer				*m_pCurrentPlayer;	// The player we are doing lag compensation for

	float					m_flTeleportDistanceSqr;

	bool					m_isCurrentlyDoingCompensation;	// Sentinel to prevent calling StartLagCompensation a second time before a Finish.

	CUtlRBTree< EHANDLE >	m_rbAdditionalEntities;
};

static CLagCompensationManager g_LagCompensationManager( "CLagCompensationManager" );
ILagCompensationManager *lagcompensation = &g_LagCompensationManager;


//-----------------------------------------------------------------------------
// Purpose: Called once per frame after all entities have had a chance to think
//-----------------------------------------------------------------------------
void CLagCompensationManager::FrameUpdatePostEntityThink()
{
	if ( (gpGlobals->maxClients <= 1) || !sv_unlag.GetBool() )
	{
		ClearHistory();
		return;
	}
	
	m_flTeleportDistanceSqr = sv_lagcompensation_teleport_dist.GetFloat() * sv_lagcompensation_teleport_dist.GetFloat();

	VPROF_BUDGET( "FrameUpdatePostEntityThink", "CLagCompensationManager" );

	CUtlRBTree< CBaseEntity * > rbTrackedEntities( DefLessFunc( CBaseEntity * ) );

	// Add active players
	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex( i );
		if ( !pPlayer )
			continue;

		if ( rbTrackedEntities.Find( pPlayer ) == rbTrackedEntities.InvalidIndex() )
		{
			rbTrackedEntities.Insert( pPlayer );
		}
	}

	// Add any additional entities
	FOR_EACH_RBTREE_FAST( m_rbAdditionalEntities, i )
	{
		CBaseEntity *pEntity = m_rbAdditionalEntities[ i ];
		if ( !pEntity )
			continue;

		if ( rbTrackedEntities.Find( pEntity ) == rbTrackedEntities.InvalidIndex() )
		{
			rbTrackedEntities.Insert( pEntity );
		}
	}

	// remove all records before that time:
	int flDeadtime = gpGlobals->curtime - sv_maxunlag.GetFloat();

	// Now record the actual history information
	FOR_EACH_UTLRBTREE( rbTrackedEntities, i )
	{
		CBaseEntity *pEntity = rbTrackedEntities[ i ];

		int idx = m_mapCompensatedEntities.Find( pEntity );
		if ( idx == m_mapCompensatedEntities.InvalidIndex() )
		{
			idx = m_mapCompensatedEntities.Insert( pEntity, new entitylagdata_t{ /*.bRestoreEntity =*/ false } );
		}
		CUtlFixedLinkedList< LagRecord > *track = &m_mapCompensatedEntities[ idx ]->RecordTrack;

		if ( !pEntity )
		{
			if ( track->Count() > 0 )
			{
				track->RemoveAll();
			}

			continue;
		}

		Assert( track->Count() < 1000 ); // insanity check

		// remove tail records that are too old
		intp tailIndex = track->Tail();
		while ( track->IsValidIndex( tailIndex ) )
		{
			LagRecord &tail = track->Element( tailIndex );

			// if tail is within limits, stop
			if ( tail.m_flSimulationTime >= flDeadtime )
				break;
			
			// remove tail, get new tail
			track->Remove( tailIndex );
			tailIndex = track->Tail();
		}

		// check if head has same simulation time
		if ( track->Count() > 0 )
		{
			LagRecord &head = track->Element( track->Head() );

			// check if player changed simulation time since last time updated
			if ( head.m_flSimulationTime >= pEntity->GetSimulationTime() )
				continue; // don't add new entry for same or older time
		}

		// add new record to player track
		LagRecord &record = track->Element( track->AddToHead() );

		record.m_fFlags = 0;
		if ( pEntity->IsAlive() )
		{
			record.m_fFlags |= LC_ALIVE;
		}

		record.m_flSimulationTime	= pEntity->GetSimulationTime();
		record.m_vecAngles			= pEntity->GetLocalAngles();
		record.m_vecOrigin			= pEntity->GetLocalOrigin();
		record.m_vecMinsPreScaled	= pEntity->CollisionProp()->OBBMinsPreScaled();
		record.m_vecMaxsPreScaled	= pEntity->CollisionProp()->OBBMaxsPreScaled();

		CBaseAnimating *pAnimating = pEntity->GetBaseAnimating();
		if ( pAnimating )
		{
			CBaseAnimatingOverlay *pOverlay = dynamic_cast< CBaseAnimatingOverlay * >( pAnimating );
			if ( pOverlay )
			{
				int layerCount = pOverlay->GetNumAnimOverlays();
				for ( int layerIndex = 0; layerIndex < layerCount; ++layerIndex )
				{
					CAnimationLayer *currentLayer = pOverlay->GetAnimOverlay( layerIndex );
					if ( currentLayer )
					{
						record.m_layerRecords[ layerIndex ].m_cycle = currentLayer->m_flCycle;
						record.m_layerRecords[ layerIndex ].m_order = currentLayer->m_nOrder;
						record.m_layerRecords[ layerIndex ].m_sequence = currentLayer->m_nSequence;
						record.m_layerRecords[ layerIndex ].m_weight = currentLayer->m_flWeight;
					}
				}
			}
			record.m_masterSequence = pAnimating->GetSequence();
			record.m_masterCycle = pAnimating->GetCycle();

			for ( int i = 0; i < MAXSTUDIOPOSEPARAM; i++ )
			{
				record.m_flPoseParameters[ i ] = pAnimating->GetPoseParameter( i );
			}
		}
	}

	//Clear the current player.
	m_pCurrentPlayer = NULL;
}

// Called during player movement to set up/restore after lag compensation
void CLagCompensationManager::StartLagCompensation( CBasePlayer *player, CUserCmd *cmd )
{
	Assert( !m_isCurrentlyDoingCompensation );

	//DONT LAG COMP AGAIN THIS FRAME IF THERES ALREADY ONE IN PROGRESS
	//IF YOU'RE HITTING THIS THEN IT MEANS THERES A CODE BUG
	if ( m_pCurrentPlayer )
	{
		Assert( m_pCurrentPlayer == NULL );
		Warning( "Trying to start a new lag compensation session while one is already active!\n" );
		return;
	}

	// Assume no entities need to be restored
	FOR_EACH_MAP_BACK( m_mapCompensatedEntities, i )
	{
		entitylagdata_t *pLagData = m_mapCompensatedEntities[ i ];

		// Wipe any deleted entities from the list
		EHANDLE hKey = m_mapCompensatedEntities.Key( i );
		if ( !hKey )
		{
			delete pLagData;
			m_mapCompensatedEntities.RemoveAt( i );
			continue;
		}

		// Clear state
		pLagData->bRestoreEntity = false;
		Q_memset( &pLagData->RestoreData, 0, sizeof( LagRecord ) );
		Q_memset( &pLagData->ChangeData, 0, sizeof( LagRecord ) );
	}

	m_bNeedToRestore = false;

	m_pCurrentPlayer = player;
	
	if ( !player->m_bLagCompensation		// Player not wanting lag compensation
		 || (gpGlobals->maxClients <= 1)	// no lag compensation in single player
		 || !sv_unlag.GetBool()				// disabled by server admin
		 || player->IsBot() 				// not for bots
		 || player->IsObserver()			// not for spectators
		)
		return;

	// NOTE: Put this here so that it won't show up in single player mode.
	VPROF_BUDGET( "StartLagCompensation", VPROF_BUDGETGROUP_OTHER_NETWORKING );

	m_isCurrentlyDoingCompensation = true;

	// Get true latency
	float flLatency = 0.f;
	INetChannelInfo *nci = engine->GetPlayerNetInfo( player->entindex() );

	if ( nci )
	{
		// add network latency
		flLatency = nci->GetLatency( FLOW_OUTGOING );
	}

	auto lambdaCalcTargetTick = [ & ]( int nLerpTicks )
	{
		// correct is the amout of time we have to correct game time
		float correct = flLatency;

		// add view interpolation latency see C_BaseEntity::GetInterpolationAmount()
		correct += TICKS_TO_TIME( nLerpTicks );

		// check bouns [0,sv_maxunlag]
		correct = Clamp( correct, 0.0f, sv_maxunlag.GetFloat() );

		// correct tick send by player 
		int targettick = cmd->tick_count - nLerpTicks;

		// calc difference between tick send by player and our latency based tick
		float deltaTime = correct - TICKS_TO_TIME( gpGlobals->tickcount - targettick );

		if ( fabs( deltaTime ) > 0.2f )
		{
			// difference between cmd time and latency is too big > 200ms, use time correction based on latency
			// DevMsg("StartLagCompensation: delta too big (%.3f)\n", deltaTime );
			targettick = gpGlobals->tickcount - TIME_TO_TICKS( correct );
		}

		return targettick;
	};

	float flBaseTargetTime = TICKS_TO_TIME( lambdaCalcTargetTick( TIME_TO_TICKS( player->m_fLerpTime ) ) );
	float flNpcTargetTime  = TICKS_TO_TIME( lambdaCalcTargetTick( TIME_TO_TICKS( player->m_fNpcLerpTime ) ) );
	
	// Iterate all compensatable entities
	const CBitVec<MAX_EDICTS> *pEntityTransmitBits = engine->GetEntityTransmitBitsForClient( player->entindex() - 1 );
	FOR_EACH_MAP( m_mapCompensatedEntities, i )
	{
		entitylagdata_t *pLagData = m_mapCompensatedEntities[ i ];
		CBaseEntity *pEntity = m_mapCompensatedEntities.Key( i );
		if ( !pEntity )
		{
			continue;
		}

		// Don't lag compensate yourself you loser...
		if ( player == pEntity )
		{
			continue;
		}

		// Custom checks for if things should lag compensate (based on things like what team the player is on).
		if ( !player->WantsLagCompensationOnEntity( pEntity, cmd, pEntityTransmitBits ) )
			continue;

		// Move other player back in time
#ifdef NEXT_BOT
		bool bIsNPC = ( pEntity->IsNPC() || ( !pEntity->IsPlayer() && pEntity->IsNextBot() ) );
#else
		bool bIsNPC = ( pEntity->IsNPC() );
#endif //NEXT_BOT
		BacktrackEntity( pEntity, pLagData, bIsNPC ? flNpcTargetTime : flBaseTargetTime );
	}
}

void CLagCompensationManager::BacktrackEntity( CBaseEntity *pEntity, entitylagdata_t *pLagData, float flTargetTime )
{
	Vector org;
	Vector minsPreScaled;
	Vector maxsPreScaled;
	QAngle ang;

	VPROF_BUDGET( "BacktrackPlayer", "CLagCompensationManager" );

	// get track history of this player
	CUtlFixedLinkedList< LagRecord > *track = &pLagData->RecordTrack;

	// check if we have at leat one entry
	if ( track->Count() <= 0 )
		return;

	intp curr = track->Head();

	LagRecord *prevRecord = NULL;
	LagRecord *record = NULL;

	Vector prevOrg = pEntity->GetLocalOrigin();
	
	// Walk context looking for any invalidating event
	while( track->IsValidIndex(curr) )
	{
		// remember last record
		prevRecord = record;

		// get next record
		record = &track->Element( curr );

		if ( !(record->m_fFlags & LC_ALIVE) )
		{
			// player most be alive, lost track
			return;
		}

		Vector delta = record->m_vecOrigin - prevOrg;
		if ( delta.Length2DSqr() > m_flTeleportDistanceSqr )
		{
			// lost track, too much difference
			return; 
		}

		// did we find a context smaller than target time ?
		if ( record->m_flSimulationTime <= flTargetTime )
			break; // hurra, stop

		prevOrg = record->m_vecOrigin;

		// go one step back
		curr = track->Next( curr );
	}

	Assert( record );

	if ( !record )
	{
		if ( sv_unlag_debug.GetBool() )
		{
			DevMsg( "No valid positions in history for BacktrackPlayer client ( %i )\n", pEntity->entindex() );
		}

		return; // that should never happen
	}

	float frac = 0.0f;
	if ( prevRecord && 
		 (record->m_flSimulationTime < flTargetTime) &&
		 (record->m_flSimulationTime < prevRecord->m_flSimulationTime) )
	{
		// we didn't find the exact time but have a valid previous record
		// so interpolate between these two records;

		Assert( prevRecord->m_flSimulationTime > record->m_flSimulationTime );
		Assert( flTargetTime < prevRecord->m_flSimulationTime );

		// calc fraction between both records
		frac = ( flTargetTime - record->m_flSimulationTime ) / 
			( prevRecord->m_flSimulationTime - record->m_flSimulationTime );

		Assert( frac > 0 && frac < 1 ); // should never extrapolate

		ang				= Lerp( frac, record->m_vecAngles, prevRecord->m_vecAngles );
		org				= Lerp( frac, record->m_vecOrigin, prevRecord->m_vecOrigin );
		minsPreScaled	= Lerp( frac, record->m_vecMinsPreScaled, prevRecord->m_vecMinsPreScaled );
		maxsPreScaled	= Lerp( frac, record->m_vecMaxsPreScaled, prevRecord->m_vecMaxsPreScaled );
	}
	else
	{
		// we found the exact record or no other record to interpolate with
		// just copy these values since they are the best we have
		org				= record->m_vecOrigin;
		ang				= record->m_vecAngles;
		minsPreScaled	= record->m_vecMinsPreScaled;
		maxsPreScaled	= record->m_vecMaxsPreScaled;
	}

	// See if this is still a valid position for us to teleport to
	if ( sv_unlag_fixstuck.GetBool() )
	{
		// Try to move to the wanted position from our current position.
		trace_t tr;
		LC_TraceEntity( pEntity, org, org, NULL, &tr );
		if ( tr.startsolid || tr.allsolid )
		{
			if ( sv_unlag_debug.GetBool() )
				DevMsg( "WARNING: BackupPlayer trying to back player into a bad position - client %i\n", pEntity->entindex() );

			// don't lag compensate the current player
			if ( tr.m_pEnt && ( tr.m_pEnt != m_pCurrentPlayer ) )
			{
				int idx = m_mapCompensatedEntities.Find( tr.m_pEnt );
				if ( idx != m_mapCompensatedEntities.InvalidIndex() )
				{
					entitylagdata_t *pTargetLagData = m_mapCompensatedEntities[ idx ];
					// If we haven't backtracked this player, do it now
					// this deliberately ignores WantsLagCompensationOnEntity.
					if ( !pTargetLagData->bRestoreEntity )
					{
						// Temp turn this flag on
						pTargetLagData->bRestoreEntity = true;

						BacktrackEntity( tr.m_pEnt, pTargetLagData, flTargetTime );

						// Remove the temp flag
						pTargetLagData->bRestoreEntity = false;
					}
				}			
			}

			// now trace us back as far as we can go
			LC_TraceEntity( pEntity, pEntity->GetLocalOrigin(), org, NULL, &tr );

			if ( tr.startsolid || tr.allsolid )
			{
				// Our starting position is bogus

				if ( sv_unlag_debug.GetBool() )
					DevMsg( "Backtrack failed completely, bad starting position\n" );
			}
			else
			{
				// We can get to a valid place, but not all the way to the target
				Vector vPos;
				VectorLerp( pEntity->GetLocalOrigin(), org, tr.fraction * g_flFractionScale, vPos );
				
				// This is as close as we're going to get
				org = vPos;

				if ( sv_unlag_debug.GetBool() )
					DevMsg( "Backtrack got most of the way\n" );
			}
		}
	}
	
	// See if this represents a change for the player
	int flags = 0;
	LagRecord *restore = &pLagData->RestoreData;
	LagRecord *change  = &pLagData->ChangeData;

	QAngle angdiff = pEntity->GetLocalAngles() - ang;
	Vector orgdiff = pEntity->GetLocalOrigin() - org;

	// Always remember the pristine simulation time in case we need to restore it.
	restore->m_flSimulationTime = pEntity->GetSimulationTime();

	if ( angdiff.LengthSqr() > LAG_COMPENSATION_EPS_SQR )
	{
		flags |= LC_ANGLES_CHANGED;
		restore->m_vecAngles = pEntity->GetLocalAngles();
		pEntity->SetLocalAngles( ang );
		change->m_vecAngles = ang;
	}

	// Use absolute equality here
	if ( minsPreScaled != pEntity->CollisionProp()->OBBMinsPreScaled() || maxsPreScaled != pEntity->CollisionProp()->OBBMaxsPreScaled() )
	{
		flags |= LC_SIZE_CHANGED;

		restore->m_vecMinsPreScaled = pEntity->CollisionProp()->OBBMinsPreScaled();
		restore->m_vecMaxsPreScaled = pEntity->CollisionProp()->OBBMaxsPreScaled();
		
		pEntity->SetSize( minsPreScaled, maxsPreScaled );
		
		change->m_vecMinsPreScaled = minsPreScaled;
		change->m_vecMaxsPreScaled = maxsPreScaled;
	}

	// Note, do origin at end since it causes a relink into the k/d tree
	if ( orgdiff.LengthSqr() > LAG_COMPENSATION_EPS_SQR )
	{
		flags |= LC_ORIGIN_CHANGED;
		restore->m_vecOrigin = pEntity->GetLocalOrigin();
		pEntity->SetLocalOrigin( org );
		change->m_vecOrigin = org;
	}

	CBaseAnimating *pAnimating = pEntity->GetBaseAnimating();
	if ( pAnimating )
	{
		// Sorry for the loss of the optimization for the case of people
		// standing still, but you breathe even on the server.
		// This is quicker than actually comparing all bazillion floats.
		flags |= LC_ANIMATION_CHANGED;
		restore->m_masterSequence = pAnimating->GetSequence();
		restore->m_masterCycle = pAnimating->GetCycle();

		bool interpolationAllowed = false;
		if ( prevRecord && ( record->m_masterSequence == prevRecord->m_masterSequence ) )
		{
			// If the master state changes, all layers will be invalid too, so don't interp (ya know, interp barely ever happens anyway)
			interpolationAllowed = true;
		}

		////////////////////////
		// First do the master settings
		bool interpolatedMasters = false;
		if ( frac > 0.0f && interpolationAllowed )
		{
			interpolatedMasters = true;
			pAnimating->SetSequence( Lerp( frac, record->m_masterSequence, prevRecord->m_masterSequence ) );
			pAnimating->SetCycle( Lerp( frac, record->m_masterCycle, prevRecord->m_masterCycle ) );

			if ( record->m_masterCycle > prevRecord->m_masterCycle )
			{
				// the older record is higher in frame than the newer, it must have wrapped around from 1 back to 0
				// add one to the newer so it is lerping from .9 to 1.1 instead of .9 to .1, for example.
				float newCycle = Lerp( frac, record->m_masterCycle, prevRecord->m_masterCycle + 1 );
				pAnimating->SetCycle( newCycle < 1 ? newCycle : newCycle - 1 );// and make sure .9 to 1.2 does not end up 1.05
			}
			else
			{
				pAnimating->SetCycle( Lerp( frac, record->m_masterCycle, prevRecord->m_masterCycle ) );
			}

			for ( int i = 0; i < MAXSTUDIOPOSEPARAM; i++ )
			{
				//don't lerp pose params, just pick the closest
				pAnimating->SetPoseParameter( i, record->m_flPoseParameters[ i ] );
				//pAnimating->SetPoseParameter( i, Lerp( frac, record->m_flPoseParameters[i], prevRecord->m_flPoseParameters[i] ) );
			}
		}
		if ( !interpolatedMasters )
		{
			pAnimating->SetSequence( record->m_masterSequence );
			pAnimating->SetCycle( record->m_masterCycle );

			for ( int i = 0; i < MAXSTUDIOPOSEPARAM; i++ )
			{
				pAnimating->SetPoseParameter( i, record->m_flPoseParameters[ i ] );
			}
		}

		////////////////////////
		// Now do all the layers
		CBaseAnimatingOverlay *pOverlay = dynamic_cast< CBaseAnimatingOverlay * >( pAnimating );
		if ( pOverlay )
		{
			int layerCount = pOverlay->GetNumAnimOverlays();
			for ( int layerIndex = 0; layerIndex < layerCount; ++layerIndex )
			{
				CAnimationLayer *currentLayer = pOverlay->GetAnimOverlay( layerIndex );
				if ( currentLayer )
				{
					restore->m_layerRecords[ layerIndex ].m_cycle = currentLayer->m_flCycle;
					restore->m_layerRecords[ layerIndex ].m_order = currentLayer->m_nOrder;
					restore->m_layerRecords[ layerIndex ].m_sequence = currentLayer->m_nSequence;
					restore->m_layerRecords[ layerIndex ].m_weight = currentLayer->m_flWeight;

					bool interpolated = false;
					if ( ( frac > 0.0f ) && interpolationAllowed )
					{
						LayerRecord &recordsLayerRecord = record->m_layerRecords[ layerIndex ];
						LayerRecord &prevRecordsLayerRecord = prevRecord->m_layerRecords[ layerIndex ];
						if ( ( recordsLayerRecord.m_order == prevRecordsLayerRecord.m_order )
							&& ( recordsLayerRecord.m_sequence == prevRecordsLayerRecord.m_sequence )
							)
						{
							// We can't interpolate across a sequence or order change
							interpolated = true;
							if ( recordsLayerRecord.m_cycle > prevRecordsLayerRecord.m_cycle )
							{
								// the older record is higher in frame than the newer, it must have wrapped around from 1 back to 0
								// add one to the newer so it is lerping from .9 to 1.1 instead of .9 to .1, for example.
								float newCycle = Lerp( frac, recordsLayerRecord.m_cycle, prevRecordsLayerRecord.m_cycle + 1 );
								currentLayer->m_flCycle = newCycle < 1 ? newCycle : newCycle - 1;// and make sure .9 to 1.2 does not end up 1.05
							}
							else
							{
								currentLayer->m_flCycle = Lerp( frac, recordsLayerRecord.m_cycle, prevRecordsLayerRecord.m_cycle );
							}
							currentLayer->m_nOrder = recordsLayerRecord.m_order;
							currentLayer->m_nSequence = recordsLayerRecord.m_sequence;
							currentLayer->m_flWeight = Lerp( frac, recordsLayerRecord.m_weight, prevRecordsLayerRecord.m_weight );
						}
					}
					if ( !interpolated )
					{
						//Either no interp, or interp failed.  Just use record.
						currentLayer->m_flCycle = record->m_layerRecords[ layerIndex ].m_cycle;
						currentLayer->m_nOrder = record->m_layerRecords[ layerIndex ].m_order;
						currentLayer->m_nSequence = record->m_layerRecords[ layerIndex ].m_sequence;
						currentLayer->m_flWeight = record->m_layerRecords[ layerIndex ].m_weight;
					}
				}
			}
		}
	}

	if ( !flags )
		return; // we didn't change anything

	if ( pAnimating && sv_lagflushbonecache.GetBool() )
		pAnimating->InvalidateBoneCache();

	/*char text[256]; Q_snprintf( text, sizeof(text), "time %.2f", flTargetTime );
	pPlayer->DrawServerHitboxes( 10 );
	NDebugOverlay::Text( org, text, false, 10 );
	NDebugOverlay::EntityBounds( pPlayer, 255, 0, 0, 32, 10 ); */

	pLagData->bRestoreEntity = true; //remember that we changed this entity
	m_bNeedToRestore = true;  // we changed at least one entity
	restore->m_fFlags = flags; // we need to restore these flags
	change->m_fFlags = flags; // we have changed these flags

	if ( pAnimating && sv_showlagcompensation.GetInt() == 1 )
	{
		pAnimating->DrawServerHitboxes( 4, true );
	}
}


void CLagCompensationManager::FinishLagCompensation( CBasePlayer *player )
{
	VPROF_BUDGET_FLAGS( "FinishLagCompensation", VPROF_BUDGETGROUP_OTHER_NETWORKING, BUDGETFLAG_CLIENT|BUDGETFLAG_SERVER );

	m_pCurrentPlayer = NULL;

	if ( !m_bNeedToRestore )
	{
		m_isCurrentlyDoingCompensation = false;
		return; // no entity was changed at all
	}

	// Iterate all active entities
	FOR_EACH_MAP( m_mapCompensatedEntities, i )
	{
		entitylagdata_t *pLagData = m_mapCompensatedEntities[ i ];
		
		if ( !pLagData->bRestoreEntity )
		{
			// entity wasn't changed by lag compensation
			continue;
		}

		CBaseEntity *pEntity = m_mapCompensatedEntities.Key( i );
		if ( !pEntity )
		{
			continue;
		}

		LagRecord *restore = &pLagData->RestoreData;
		LagRecord *change  = &pLagData->ChangeData;

		bool restoreSimulationTime = false;

		if ( restore->m_fFlags & LC_SIZE_CHANGED )
		{
			restoreSimulationTime = true;
	
			// see if simulation made any changes, if no, then do the restore, otherwise,
			//  leave new values in
			if ( pEntity->CollisionProp()->OBBMinsPreScaled() == change->m_vecMinsPreScaled &&
				pEntity->CollisionProp()->OBBMaxsPreScaled() == change->m_vecMaxsPreScaled )
			{
				// Restore it
				pEntity->SetSize( restore->m_vecMinsPreScaled, restore->m_vecMaxsPreScaled );
			}
		}

		if ( restore->m_fFlags & LC_ANGLES_CHANGED )
		{		   
			restoreSimulationTime = true;

			if ( pEntity->GetLocalAngles() == change->m_vecAngles )
			{
				pEntity->SetLocalAngles( restore->m_vecAngles );
			}
		}

		if ( restore->m_fFlags & LC_ORIGIN_CHANGED )
		{
			restoreSimulationTime = true;

			// Okay, let's see if we can do something reasonable with the change
			Vector delta = pEntity->GetLocalOrigin() - change->m_vecOrigin;
			
			// If it moved really far, just leave the player in the new spot!!!
			if ( delta.Length2DSqr() < m_flTeleportDistanceSqr )
			{
				RestoreEntityTo( pEntity, restore->m_vecOrigin + delta );
			}
		}

		CBaseAnimating *pAnimating = pEntity->GetBaseAnimating();
		if( pAnimating && restore->m_fFlags & LC_ANIMATION_CHANGED )
		{
			restoreSimulationTime = true;

			pAnimating->SetSequence(restore->m_masterSequence);
			pAnimating->SetCycle(restore->m_masterCycle);

			CBaseAnimatingOverlay *pOverlay = dynamic_cast< CBaseAnimatingOverlay * >( pAnimating );
			if ( pOverlay )
			{
				int layerCount = pOverlay->GetNumAnimOverlays();
				for ( int layerIndex = 0; layerIndex < layerCount; ++layerIndex )
				{
					CAnimationLayer *currentLayer = pOverlay->GetAnimOverlay( layerIndex );
					if ( currentLayer )
					{
						currentLayer->m_flCycle = restore->m_layerRecords[ layerIndex ].m_cycle;
						currentLayer->m_nOrder = restore->m_layerRecords[ layerIndex ].m_order;
						currentLayer->m_nSequence = restore->m_layerRecords[ layerIndex ].m_sequence;
						currentLayer->m_flWeight = restore->m_layerRecords[ layerIndex ].m_weight;
					}
				}
			}

			for( int i=0; i<MAXSTUDIOPOSEPARAM; i++ )
			{
				pAnimating->SetPoseParameter( i, restore->m_flPoseParameters[i] );
			}
		}

		if ( restoreSimulationTime )
		{
			pEntity->SetSimulationTime( restore->m_flSimulationTime );
		}
	}

	m_isCurrentlyDoingCompensation = false;
}

// Mappers can flag certain additional entities to lag compensate, this handles them
void CLagCompensationManager::AddAdditionalEntity( EHANDLE hEntity )
{
	if ( m_rbAdditionalEntities.Find( hEntity ) == m_rbAdditionalEntities.InvalidIndex() )
	{
		m_rbAdditionalEntities.Insert( hEntity );
	}
}

void CLagCompensationManager::RemoveAdditionalEntity( EHANDLE hEntity )
{
	m_rbAdditionalEntities.Remove( hEntity );
}
