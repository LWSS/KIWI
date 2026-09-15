#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include <universal/q_shared.h>
#include "server.h"
#include <client/cl_parse.h>
#include "sv_game.h"
#include <bgame/bg_public.h>
#include <qcommon/msg.h>
#include <game/g_local.h>
#include <client/cl_demo.h>
#include <client/client.h>


/*
==================
SV_WriteSnapshotToClient
==================
*/
void __cdecl SV_WriteSnapshotToClient(client_t *client, msg_t *msg)
{
    int Time; // r3
    int numSnapshotEntities; // r7
    int v6; // r29
    int *snapshotEntities; // r31

    MSG_WriteByte(msg, 2);
    // send over the current server time so the client can drift
    // its view of time to try to match
    Time = G_GetTime();
    MSG_WriteLong(msg, Time);
    if (client->state != 1)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\server\\sv_snapshot.cpp",
            49,
            0,
            "%s\n\t(client->state) = %i",
            "(client->state == CS_ACTIVE)",
            client->state);
    MSG_WriteByte(msg, svs.snapFlagServerBit);
    // delta encode the playerstate
    MSG_WriteDeltaPlayerstate(msg, client->frames);
    // delta encode the entities
    numSnapshotEntities = sv.entityNumbers.numSnapshotEntities;
    if (sv.entityNumbers.numSnapshotEntities > 0x800u)
    {
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\server\\sv_snapshot.cpp",
            56,
            0,
            "sv.entityNumbers.numSnapshotEntities not in [0, MAX_SNAPSHOT_ENTITIES]\n\t%i not in [%i, %i]",
            sv.entityNumbers.numSnapshotEntities,
            0,
            2048);
        numSnapshotEntities = sv.entityNumbers.numSnapshotEntities;
    }
    v6 = 0;
    if (numSnapshotEntities > 0)
    {
        snapshotEntities = sv.entityNumbers.snapshotEntities;
        do
        {
            bcassert((unsigned int)*snapshotEntities, 0x880);
            MSG_WriteBits(msg, *snapshotEntities, 12);
            ++v6;
            ++snapshotEntities;
        } while (v6 < sv.entityNumbers.numSnapshotEntities);
    }
    MSG_WriteBits(msg, ENTITYNUM_NONE, 12);
}

/*
==================
SV_UpdateServerCommandsToClient

(re)send all server commands the client hasn't acknowledged yet
==================
*/
void __cdecl SV_UpdateServerCommandsToClient(client_t *client)
{
    // write any unacknowledged serverCommands
    CL_ParseCommandString(&client->reliableCommands);
    client->reliableCommands.header.sent = client->reliableCommands.header.sequence;
    client->reliableCommands.header.rover = 0;
}

/*
===============
SV_AddEntToSnapshot
===============
*/
void __cdecl SV_AddEntToSnapshot(int entnum)
{
    int numSnapshotEntities; // r11

    numSnapshotEntities = sv.entityNumbers.numSnapshotEntities;
    // if we are full, silently discard entities
    if ((unsigned int)numSnapshotEntities >= ARRAY_COUNT(sv.entityNumbers.snapshotEntities))
    {
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\server\\sv_snapshot.cpp",
            95,
            0,
            "%s",
            "sv.entityNumbers.numSnapshotEntities != MAX_SNAPSHOT_ENTITIES");
        return;
    }
    sv.entityNumbers.snapshotEntities[numSnapshotEntities] = entnum;
    ++sv.entityNumbers.numSnapshotEntities;
}

/*
===============
SV_AddEntitiesVisibleFromPoint
===============
*/
void __cdecl SV_AddEntitiesVisibleFromPoint(int clientNum)
{
    int e; // r30
    gentity_s *ent; // r3
    gentity_s *v4; // r31
    int number; // r4
    const char *v6; // r3

    if (sv.state)
    {
        for (e = 0; e < sv.num_entities; ++e)
        {
            ent = SV_GentityNum(e);
            v4 = ent;
            // never send entities that aren't linked in
            if (ent->r.linked)
            {
                number = ent->s.number;
                iassert(ent->s.number == e);
                // entities can be flagged to explicitly not be sent to the client
                if ((v4->r.svFlags & 1) == 0 && e != clientNum)
                    // add it
                    SV_AddEntToSnapshot(e);
            }
        }
    }
}

/*
=============
SV_BuildClientSnapshot

Decides which entities are going to be visible to the client, and
copies off the playerstate and areabits.

This properly handles multiple recursive portals, but the render
currently doesn't.

For viewing through other player's eyes, clent can be something other than client->gentity
=============
*/
void __cdecl SV_BuildClientSnapshot(client_t *client)
{
    playerState_s *frames; // r31
    playerState_s *v2; // r3
    unsigned int clientNum; // r31

    if (client->gentity)
    {
        frames = client->frames;
        // clear everything in this snapshot
        sv.entityNumbers.numSnapshotEntities = 0;
        // grab the current playerState_t
        v2 = SV_GameClientNum(client - svs.clients);
        memcpy(frames, v2, sizeof(playerState_s));
        clientNum = frames->clientNum;
        if (clientNum >= 0x880)
            Com_Error(ERR_DROP, "SV_BuildClientSnapshot: bad gEnt");
        // add all the entities directly visible to the eye, which
        // may include portal entities that merge other viewpoints
        SV_AddEntitiesVisibleFromPoint(clientNum);
    }
}

/*
=======================
SV_SendMessageToClient

Called by SV_SendClientSnapshot and SV_SendClientGameState
=======================
*/
void __cdecl SV_SendMessageToClient(msg_t *msg, client_t *client)
{
    int outgoingSequence; // r4

    MSG_WriteByte(msg, 4);
    outgoingSequence = client->netchan.outgoingSequence;
    client->netchan.outgoingSequence = outgoingSequence + 1;
    // send the datagram
    CL_PacketEvent(msg, outgoingSequence);
}

void __cdecl SV_BuildAndSendClientSnapshot(client_t *client)
{
    int outgoingSequence; // r4
    msg_t msg; // [sp+50h] [-4040h] BYREF
    unsigned __int8 msgbuf[0x4000]; // [sp+80h] [-4010h] BYREF

    SV_BuildClientSnapshot(client);
    MSG_Init(&msg, msgbuf, 0x4000);
    SV_WriteSnapshotToClient(client, &msg);
    if (msg.overflowed)
        Com_Error(ERR_DROP, "SV_BuildAndSendClientSnapshot: bad gEnt");
    MSG_WriteByte(&msg, 4);
    outgoingSequence = client->netchan.outgoingSequence;
    client->netchan.outgoingSequence = outgoingSequence + 1;
    CL_PacketEvent(&msg, outgoingSequence);
}

/*
=======================
SV_SendClientMessages
=======================
*/
void __cdecl SV_SendClientMessages()
{
    client_t *clients; // r11
    client_t *v1; // r30
    serverCommands_s *p_reliableCommands; // r29

    clients = svs.clients;
    if (!svs.clients)
    {
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\server\\sv_snapshot.cpp", 216, 0, "%s", "svs.clients");
        clients = svs.clients;
    }
    if (!clients->state)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\server\\sv_snapshot.cpp", 217, 0, "%s", "svs.clients[0].state");
    // send a message to each connected client
    if (!CL_DemoPlaying())
    {
        G_SendClientMessages();
        v1 = svs.clients;
        p_reliableCommands = &svs.clients->reliableCommands;
        CL_ParseCommandString(&svs.clients->reliableCommands);
        v1->reliableCommands.header.sent = v1->reliableCommands.header.sequence;
        p_reliableCommands->header.rover = 0;
        //Profile_Begin(31);
        // generate and send a new message
        SV_BuildAndSendClientSnapshot(svs.clients);
        //Profile_EndInternal(0);
    }
}

void __cdecl SV_WriteSnapshotToClientCmd(void *cmdData)
{
    if (!alwaysfails)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\server\\sv_snapshot.cpp", 236, 0, "MP only");
}

void __cdecl SV_ArchiveSnapshotCmd(void *cmdData)
{
    if (!alwaysfails)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\server\\sv_snapshot.cpp", 242, 0, "MP only");
}

