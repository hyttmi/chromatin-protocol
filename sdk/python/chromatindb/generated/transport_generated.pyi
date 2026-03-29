from __future__ import annotations

import flatbuffers
import numpy as np

import flatbuffers
import typing

uoffset: typing.TypeAlias = flatbuffers.number_types.UOffsetTFlags.py_type

class TransportMsgType(object):
  None_: int
  KemPubkey: int
  KemCiphertext: int
  AuthSignature: int
  AuthPubkey: int
  Ping: int
  Pong: int
  Goodbye: int
  Data: int
  SyncRequest: int
  SyncAccept: int
  NamespaceList: int
  BlobRequest: int
  BlobTransfer: int
  SyncComplete: int
  PeerListRequest: int
  PeerListResponse: int
  Delete: int
  DeleteAck: int
  Subscribe: int
  Unsubscribe: int
  Notification: int
  StorageFull: int
  TrustedHello: int
  PQRequired: int
  QuotaExceeded: int
  ReconcileInit: int
  ReconcileRanges: int
  ReconcileItems: int
  SyncRejected: int
  WriteAck: int
  ReadRequest: int
  ReadResponse: int
  ListRequest: int
  ListResponse: int
  StatsRequest: int
  StatsResponse: int
  ExistsRequest: int
  ExistsResponse: int
  NodeInfoRequest: int
  NodeInfoResponse: int
  NamespaceListRequest: int
  NamespaceListResponse: int
  StorageStatusRequest: int
  StorageStatusResponse: int
  NamespaceStatsRequest: int
  NamespaceStatsResponse: int
  MetadataRequest: int
  MetadataResponse: int
  BatchExistsRequest: int
  BatchExistsResponse: int
  DelegationListRequest: int
  DelegationListResponse: int
  BatchReadRequest: int
  BatchReadResponse: int
  PeerInfoRequest: int
  PeerInfoResponse: int
  TimeRangeRequest: int
  TimeRangeResponse: int
class TransportMessage(object):
  @classmethod
  def GetRootAs(cls, buf: bytes, offset: int) -> TransportMessage: ...
  @classmethod
  def GetRootAsTransportMessage(cls, buf: bytes, offset: int) -> TransportMessage: ...
  def Init(self, buf: bytes, pos: int) -> None: ...
  def Type(self) -> typing.Literal[TransportMsgType.None_, TransportMsgType.KemPubkey, TransportMsgType.KemCiphertext, TransportMsgType.AuthSignature, TransportMsgType.AuthPubkey, TransportMsgType.Ping, TransportMsgType.Pong, TransportMsgType.Goodbye, TransportMsgType.Data, TransportMsgType.SyncRequest, TransportMsgType.SyncAccept, TransportMsgType.NamespaceList, TransportMsgType.BlobRequest, TransportMsgType.BlobTransfer, TransportMsgType.SyncComplete, TransportMsgType.PeerListRequest, TransportMsgType.PeerListResponse, TransportMsgType.Delete, TransportMsgType.DeleteAck, TransportMsgType.Subscribe, TransportMsgType.Unsubscribe, TransportMsgType.Notification, TransportMsgType.StorageFull, TransportMsgType.TrustedHello, TransportMsgType.PQRequired, TransportMsgType.QuotaExceeded, TransportMsgType.ReconcileInit, TransportMsgType.ReconcileRanges, TransportMsgType.ReconcileItems, TransportMsgType.SyncRejected, TransportMsgType.WriteAck, TransportMsgType.ReadRequest, TransportMsgType.ReadResponse, TransportMsgType.ListRequest, TransportMsgType.ListResponse, TransportMsgType.StatsRequest, TransportMsgType.StatsResponse, TransportMsgType.ExistsRequest, TransportMsgType.ExistsResponse, TransportMsgType.NodeInfoRequest, TransportMsgType.NodeInfoResponse, TransportMsgType.NamespaceListRequest, TransportMsgType.NamespaceListResponse, TransportMsgType.StorageStatusRequest, TransportMsgType.StorageStatusResponse, TransportMsgType.NamespaceStatsRequest, TransportMsgType.NamespaceStatsResponse, TransportMsgType.MetadataRequest, TransportMsgType.MetadataResponse, TransportMsgType.BatchExistsRequest, TransportMsgType.BatchExistsResponse, TransportMsgType.DelegationListRequest, TransportMsgType.DelegationListResponse, TransportMsgType.BatchReadRequest, TransportMsgType.BatchReadResponse, TransportMsgType.PeerInfoRequest, TransportMsgType.PeerInfoResponse, TransportMsgType.TimeRangeRequest, TransportMsgType.TimeRangeResponse]: ...
  def Payload(self, i: int) -> typing.List[int]: ...
  def PayloadAsNumpy(self) -> np.ndarray: ...
  def PayloadLength(self) -> int: ...
  def PayloadIsNone(self) -> bool: ...
  def RequestId(self) -> int: ...
def TransportMessageStart(builder: flatbuffers.Builder) -> None: ...
def TransportMessageAddType(builder: flatbuffers.Builder, type: typing.Literal[TransportMsgType.None_, TransportMsgType.KemPubkey, TransportMsgType.KemCiphertext, TransportMsgType.AuthSignature, TransportMsgType.AuthPubkey, TransportMsgType.Ping, TransportMsgType.Pong, TransportMsgType.Goodbye, TransportMsgType.Data, TransportMsgType.SyncRequest, TransportMsgType.SyncAccept, TransportMsgType.NamespaceList, TransportMsgType.BlobRequest, TransportMsgType.BlobTransfer, TransportMsgType.SyncComplete, TransportMsgType.PeerListRequest, TransportMsgType.PeerListResponse, TransportMsgType.Delete, TransportMsgType.DeleteAck, TransportMsgType.Subscribe, TransportMsgType.Unsubscribe, TransportMsgType.Notification, TransportMsgType.StorageFull, TransportMsgType.TrustedHello, TransportMsgType.PQRequired, TransportMsgType.QuotaExceeded, TransportMsgType.ReconcileInit, TransportMsgType.ReconcileRanges, TransportMsgType.ReconcileItems, TransportMsgType.SyncRejected, TransportMsgType.WriteAck, TransportMsgType.ReadRequest, TransportMsgType.ReadResponse, TransportMsgType.ListRequest, TransportMsgType.ListResponse, TransportMsgType.StatsRequest, TransportMsgType.StatsResponse, TransportMsgType.ExistsRequest, TransportMsgType.ExistsResponse, TransportMsgType.NodeInfoRequest, TransportMsgType.NodeInfoResponse, TransportMsgType.NamespaceListRequest, TransportMsgType.NamespaceListResponse, TransportMsgType.StorageStatusRequest, TransportMsgType.StorageStatusResponse, TransportMsgType.NamespaceStatsRequest, TransportMsgType.NamespaceStatsResponse, TransportMsgType.MetadataRequest, TransportMsgType.MetadataResponse, TransportMsgType.BatchExistsRequest, TransportMsgType.BatchExistsResponse, TransportMsgType.DelegationListRequest, TransportMsgType.DelegationListResponse, TransportMsgType.BatchReadRequest, TransportMsgType.BatchReadResponse, TransportMsgType.PeerInfoRequest, TransportMsgType.PeerInfoResponse, TransportMsgType.TimeRangeRequest, TransportMsgType.TimeRangeResponse]) -> None: ...
def TransportMessageAddPayload(builder: flatbuffers.Builder, payload: uoffset) -> None: ...
def TransportMessageStartPayloadVector(builder: flatbuffers.Builder, num_elems: int) -> uoffset: ...
def TransportMessageAddRequestId(builder: flatbuffers.Builder, requestId: int) -> None: ...
def TransportMessageEnd(builder: flatbuffers.Builder) -> uoffset: ...

