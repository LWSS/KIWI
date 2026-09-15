#ifndef KISAK_MP
#error This File is MultiPlayer Only
#endif

#include <universal/q_shared.h>
#include "server_mp.h"

#include <cstring>
#include <qcommon/net_chan_mp.h>

/*
==============
SV_Netchan_Decode

	// first 12 bytes of the data are always:
	long serverId;
	long messageAcknowledge;
	long reliableAcknowledge;

==============
*/
void __cdecl SV_Netchan_Decode(client_t *client, uint8_t *data, int size)
{
    int i, index;
    byte key, * string;

    iassert(client->reliableSequence - client->reliableAcknowledge < MAX_RELIABLE_COMMANDS);

    string = (byte*)client->reliableCommandInfo[client->reliableAcknowledge & (MAX_RELIABLE_COMMANDS - 1)].cmd;
    // xor the client challenge with the netchan sequence number
    key = client->challenge ^ (byte)client->serverId ^ client->messageAcknowledge;

    // decode the data with this key
    for (i = 0, index = 0; i < size; i++)
    {
        if (!string[index])
        {
            index = 0;
        }

        iassert(string[index] != '%');

        // modify the key with the last sent and acknowledged server command
        key ^= string[index] << (i & 1);
        data[i] ^= key;

        index++;
    }
}

/*
==============
SV_Netchan_Encode

	// first four bytes of the data are always:
	long reliableAcknowledge;

==============
*/
void __cdecl SV_Netchan_Encode(client_t *client, uint8_t *data, int size)
{
    int i, index;
    byte key, * string;

    // modify the key with the last received and with this message acknowledged client command
    string = (byte*)client->lastClientCommandString;
    // xor the client challenge with the netchan sequence number
    key = client->challenge ^ client->header.netchan.outgoingSequence;

    // encode the data with this key
    for (i = 0, index = 0; i < size; i++)
    {
        if (!string[index])
        {
            index = 0;
        }

        iassert(string[index] != '%');

        // modify the key with the last sent and acknowledged server command
        key ^= string[index] << (i & 1);
        data[i] ^= key;

        index++;
    }
}

void __cdecl SV_Netchan_OutgoingSequenceIncremented(client_t *client, netchan_t *chan)
{
    clientSnapshot_t *frame; // [esp+0h] [ebp-4h]

    frame = &client->frames[chan->outgoingSequence & 0x1F];
    memset(frame, 0, sizeof(clientSnapshot_t));
    frame->first_entity = svs.nextSnapshotEntities;
    frame->first_client = svs.nextSnapshotClients;
}

/*
=================
SV_Netchan_TransmitNextFragment
=================
*/
bool __cdecl SV_Netchan_TransmitNextFragment(client_t *client, netchan_t *chan)
{
    bool res; // [esp+3h] [ebp-1h]

    res = Netchan_TransmitNextFragment(chan);
    // the last fragment was transmitted, check wether we have queued messages
    if (!chan->unsentFragments)
        SV_Netchan_OutgoingSequenceIncremented(client, chan);
    return res;
}

/*
===============
SV_Netchan_Transmit
================
*/
bool __cdecl SV_Netchan_Transmit(client_t *client, uint8_t *data, int length)
{
    bool res; // [esp+3h] [ebp-1h]

    SV_Netchan_Encode(client, data + 4, length - 4);
    res = Netchan_Transmit(&client->header.netchan, length, (char *)data);
    if (!client->header.netchan.unsentFragments)
        SV_Netchan_OutgoingSequenceIncremented(client, &client->header.netchan);
    return res;
}

void __cdecl SV_Netchan_AddOOBProfilePacket(int iLength)
{
    if (net_profile->current.integer)
    {
        NetProf_PrepProfiling(&svs.OOBProf);
        NetProf_AddPacket(&svs.OOBProf.send, iLength, 0);
    }
}

void __cdecl SV_Netchan_UpdateProfileStats()
{
    client_t *pClient; // [esp+0h] [ebp-8h]
    int i; // [esp+4h] [ebp-4h]

    if (net_profile->current.integer)
    {
        NetProf_UpdateStatistics(&svs.OOBProf.send);
        NetProf_UpdateStatistics(&svs.OOBProf.recieve);
        i = 0;
        pClient = svs.clients;
        while (i < sv_maxclients->current.integer)
        {
            if (pClient->header.state)
            {
                NetProf_UpdateStatistics(&pClient->header.netchan.prof.send);
                NetProf_UpdateStatistics(&pClient->header.netchan.prof.recieve);
            }
            ++i;
            ++pClient;
        }
    }
}

