//========= Copyright © 1996-2009, Valve Corporation, All rights reserved. ============//
//
// Purpose: Allows movies to be played as a VGUI screen in the world
//
//=====================================================================================//

#include "cbase.h"
#include "vguiscreen.h"

// NOTE: This has to be the last file included!
#include "tier0/memdbgon.h"

class CMovieDisplay : public CBaseEntity
{
public:
	DECLARE_CLASS( CMovieDisplay, CBaseEntity );
	DECLARE_DATADESC();
	DECLARE_SERVERCLASS();

	virtual ~CMovieDisplay();

	virtual bool KeyValue( const char* szKeyName, const char* szValue );

	virtual int  UpdateTransmitState();
	virtual void SetTransmit( CCheckTransmitInfo* pInfo, bool bAlways );

	virtual void Spawn( void );
	virtual void Precache( void );
	virtual void OnRestore( void );

	void	ScreenVisible( bool bVisible );

	void	Disable( void );
	void	Enable( void );

	void	PauseMovie( void );
	void	UnpauseMovie( void );

private:
	// Control panel
	void GetControlPanelInfo( int nPanelIndex, const char*& pPanelName );
	void GetControlPanelClassName( int nPanelIndex, const char*& pPanelName );
	void SpawnControlPanels( void );
	void RestoreControlPanels( void );

private:
	CNetworkVar( bool, m_bEnabled );
	CNetworkVar( bool, m_bPlaying );
	CNetworkVar( bool, m_bAutoStart );
	CNetworkVar( bool, m_bLooping );

	// Server stores the start playback time so that clients that joined late can synchronize playback
	CNetworkVar( float, m_flStartPlaybackTime );

	// Filename of the movie to play
	CNetworkString( m_szMovieFilename, 128 );
	string_t	m_iszMovieFilename;

	// "Group" name.  Screens of the same group name will play the same movie at the same time
	// Effectively this lets multiple screens tune to the same "channel" in the world
	CNetworkString( m_szGroupName, 128 );
	string_t	m_iszGroupName;

	int			m_iScreenWidth;
	int			m_iScreenHeight;

	bool		m_bDoFullTransmit;

	CHandle< CVGuiScreen >	m_hScreen;

public:
	// Input handlers
	void	InputDisable( inputdata_t& inputdata );
	void	InputEnable( inputdata_t& inputdata );
	void	InputPause( inputdata_t& inputdata );
	void	InputUnpause( inputdata_t& inputdata );
};

LINK_ENTITY_TO_CLASS( vgui_movie_display, CMovieDisplay );

//-----------------------------------------------------------------------------
// Save/load 
//-----------------------------------------------------------------------------
BEGIN_DATADESC( CMovieDisplay )

DEFINE_FIELD( m_bEnabled, FIELD_BOOLEAN ),

DEFINE_AUTO_ARRAY( m_szMovieFilename, FIELD_CHARACTER ),
DEFINE_KEYFIELD( m_iszMovieFilename, FIELD_STRING, "moviefilename" ),

DEFINE_AUTO_ARRAY( m_szGroupName, FIELD_CHARACTER ),
DEFINE_KEYFIELD( m_iszGroupName, FIELD_STRING, "groupname" ),

DEFINE_KEYFIELD( m_iScreenWidth, FIELD_INTEGER, "width" ),
DEFINE_KEYFIELD( m_iScreenHeight, FIELD_INTEGER, "height" ),

DEFINE_KEYFIELD( m_bAutoStart, FIELD_BOOLEAN, "autostart" ),
DEFINE_KEYFIELD( m_bLooping, FIELD_BOOLEAN, "looping" ),

DEFINE_FIELD( m_bDoFullTransmit, FIELD_BOOLEAN ),

DEFINE_FIELD( m_hScreen, FIELD_EHANDLE ),

DEFINE_INPUTFUNC( FIELD_VOID, "Disable", InputDisable ),
DEFINE_INPUTFUNC( FIELD_VOID, "Enable", InputEnable ),
DEFINE_INPUTFUNC( FIELD_VOID, "PauseMovie", InputPause ),
DEFINE_INPUTFUNC( FIELD_VOID, "UnpauseMovie", InputUnpause ),

END_DATADESC()

IMPLEMENT_SERVERCLASS_ST( CMovieDisplay, DT_MovieDisplay )

SendPropBool( SENDINFO( m_bEnabled ) ),
SendPropBool( SENDINFO( m_bPlaying ) ),
SendPropBool( SENDINFO( m_bAutoStart ) ),
SendPropBool( SENDINFO( m_bLooping ) ),

SendPropFloat( SENDINFO( m_flStartPlaybackTime ), 0, SPROP_NOSCALE ),

SendPropString( SENDINFO( m_szMovieFilename ) ),
SendPropString( SENDINFO( m_szGroupName ) ),

END_SEND_TABLE()

CMovieDisplay::~CMovieDisplay()
{
	DestroyVGuiScreen( m_hScreen.Get() );
}

//-----------------------------------------------------------------------------
// Purpose: Read in Hammer data
//-----------------------------------------------------------------------------
bool CMovieDisplay::KeyValue( const char* szKeyName, const char* szValue )
{
	// Purpose: NOTE: Have to do these separate because they set two values instead of one
	if( FStrEq( szKeyName, "angles" ) )
	{
		Assert( GetMoveParent() == NULL );
		QAngle angles;
		UTIL_StringToVector( angles.Base(), szValue );

		// Purpose: Because the vgui screen basis is strange (z is front, y is up, x is right)
		// Purpose: we need to rotate the typical basis before applying it
		VMatrix mat, rotation, tmp;
		MatrixFromAngles( angles, mat );
		MatrixBuildRotationAboutAxis( rotation, Vector( 0, 1, 0 ), 90 );
		MatrixMultiply( mat, rotation, tmp );
		MatrixBuildRotateZ( rotation, 90 );
		MatrixMultiply( tmp, rotation, mat );
		MatrixToAngles( mat, angles );
		SetAbsAngles( angles );

		return true;
	}

	return BaseClass::KeyValue( szKeyName, szValue );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
int CMovieDisplay::UpdateTransmitState()
{
	if( m_bDoFullTransmit )
	{
		m_bDoFullTransmit = false;
		return SetTransmitState( FL_EDICT_ALWAYS );
	}

	return SetTransmitState( FL_EDICT_FULLCHECK );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::SetTransmit( CCheckTransmitInfo* pInfo, bool bAlways )
{
	// Purpose: Are we already marked for transmission?
	if( pInfo->m_pTransmitEdict->Get( entindex() ) )
		return;

	BaseClass::SetTransmit( pInfo, bAlways );

	// Purpose: Force our screen to be sent too.
	m_hScreen->SetTransmit( pInfo, bAlways );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::Spawn( void )
{
	// Purpose: Move the strings into a networkable form
	Q_strcpy( m_szMovieFilename.GetForModify(), m_iszMovieFilename.ToCStr() );
	Q_strcpy( m_szGroupName.GetForModify(), m_iszGroupName.ToCStr() );

	Precache();

	BaseClass::Spawn();

	m_bEnabled = false;

	m_flStartPlaybackTime = gpGlobals->curtime;

	SpawnControlPanels();

	ScreenVisible( m_bEnabled );

	m_bDoFullTransmit = true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::Precache( void )
{
	BaseClass::Precache();

	PrecacheVGuiScreen( "movie_display_screen" );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::OnRestore( void )
{
	BaseClass::OnRestore();

	RestoreControlPanels();

	ScreenVisible( m_bEnabled );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::ScreenVisible( bool bVisible )
{
	// Purpose: Set its active state
	m_hScreen->SetActive( bVisible );

	if( bVisible )
	{
		m_hScreen->RemoveEffects( EF_NODRAW );
	}
	else
	{
		m_hScreen->AddEffects( EF_NODRAW );
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::Disable( void )
{
	if( !m_bEnabled )
		return;

	m_bEnabled = false;

	ScreenVisible( false );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::Enable( void )
{
	if( m_bEnabled )
		return;

	m_bEnabled = true;

	ScreenVisible( true );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::PauseMovie( void )
{
	if( !m_bPlaying )
		return;

	m_bPlaying = false;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::UnpauseMovie( void )
{
	if( m_bPlaying )
		return;

	m_bPlaying = true;
	m_flStartPlaybackTime = gpGlobals->curtime;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::GetControlPanelInfo( int nPanelIndex, const char*& pPanelName )
{
	pPanelName = "movie_display_screen";
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::GetControlPanelClassName( int nPanelIndex, const char*& pPanelName )
{
	pPanelName = "vgui_screen";
}

//-----------------------------------------------------------------------------
// Purpose: This is called by the base object when it's time to spawn the control panels
//-----------------------------------------------------------------------------
void CMovieDisplay::SpawnControlPanels()
{
	int nPanel;
	for( nPanel = 0; true; ++nPanel )
	{
		const char* pScreenName;
		GetControlPanelInfo( nPanel, pScreenName );
		if( !pScreenName )
			continue;

		const char* pScreenClassname;
		GetControlPanelClassName( nPanel, pScreenClassname );
		if( !pScreenClassname )
			continue;

		float flWidth = m_iScreenWidth;
		float flHeight = m_iScreenHeight;

		CVGuiScreen* pScreen = CreateVGuiScreen( pScreenClassname, pScreenName, this, this, 0 );
		pScreen->ChangeTeam( GetTeamNumber() );
		pScreen->SetActualSize( flWidth, flHeight );
		pScreen->SetActive( true );
		pScreen->MakeVisibleOnlyToTeammates( false );
		pScreen->SetTransparency( true );
		m_hScreen = pScreen;

		return;
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::RestoreControlPanels( void )
{
	int nPanel;
	for( nPanel = 0; true; ++nPanel )
	{
		const char* pScreenName;
		GetControlPanelInfo( nPanel, pScreenName );
		if( !pScreenName )
			continue;

		const char* pScreenClassname;
		GetControlPanelClassName( nPanel, pScreenClassname );
		if( !pScreenClassname )
			continue;

		CVGuiScreen* pScreen = ( CVGuiScreen* )gEntList.FindEntityByClassname( NULL, pScreenClassname );

		while( ( pScreen && pScreen->GetOwnerEntity() != this ) || Q_strcmp( pScreen->GetPanelName(), pScreenName ) != 0 )
		{
			pScreen = ( CVGuiScreen* )gEntList.FindEntityByClassname( pScreen, pScreenClassname );
		}

		if( pScreen )
		{
			m_hScreen = pScreen;
			m_hScreen->SetActive( true );
		}

		return;
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::InputDisable( inputdata_t& inputdata )
{
	Disable();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::InputEnable( inputdata_t& inputdata )
{
	Enable();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::InputPause( inputdata_t& inputdata )
{
	PauseMovie();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMovieDisplay::InputUnpause( inputdata_t& inputdata )
{
	UnpauseMovie();
}
