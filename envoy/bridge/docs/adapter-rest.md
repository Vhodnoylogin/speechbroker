# envoy-rest - a sketch, not implemented

A universal adapter: a mod that registers with the bridge by the internal rules and, facing
outward, keeps an HTTP server for programs living outside the game.

## What for

An adapter has to be an SKSE plugin, which is to say C++. That cuts off everybody writing in
Python and the rest. This adapter removes the limit: it alone is written in C++, and through it
anybody can attach to the bridge without building a single DLL.

What matters is that the socket then lives **in the adapter and not in the core**. The bridge stays
with no port, no token and no attack surface; whoever needs the network enables this mod, whoever
does not, does not.

## What it must have

- an HTTP server on a configurable port, `127.0.0.1` only;
- a **token** in a header, as in MO2Bridge: otherwise any local program could send an invented
  utterance and the bridge would deal it out to the subscribers as something it heard;
- routes that mirror the interface of the bridge: `POST /v1/register`, `POST /v1/utterance`,
  `GET /v1/pending` by long polling, `GET /v1/status`;
- one registration at the bridge for the whole adapter, with the outside clients as its charges:
  it declares their capabilities under its own name.

## What it must not do

It must not carry knowledge of particular models. This adapter is universal exactly because it does
not understand the content: it carries what it was given and does not look inside.

## The state of it

Only this file. The implementation is put off: the voice adapter is being checked at the moment,
and a second unfinished component would only get in the way of seeing what exactly broke.
