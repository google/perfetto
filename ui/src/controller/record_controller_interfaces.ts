import {ConsumerPortResponse} from './consumer_port_types';

export type ConsumerPortCallback = (_: ConsumerPortResponse) => void;
export type ErrorCallback = (_: string) => void;
export type StatusCallback = (_: string) => void;

export abstract class RpcConsumerPort {
  // The responses of the call invocations should be sent through this listener.
  // This is done by the 3 "send" methods in this abstract class.
  private consumerPortListener: Consumer;

  constructor(consumerPortListener: Consumer) {
    this.consumerPortListener = consumerPortListener;
  }

  // RequestData is the proto representing the arguments of the function call.
  abstract handleCommand(methodName: string, requestData: Uint8Array): void;

  sendMessage(data: ConsumerPortResponse) {
    this.consumerPortListener.onConsumerPortResponse(data);
  }

  sendErrorMessage(message: string) {
    this.consumerPortListener.onError(message);
  }

  sendStatus(status: string) {
    this.consumerPortListener.onStatus(status);
  }
}

export interface Consumer {
  onConsumerPortResponse(data: ConsumerPortResponse): void;
  onError: ErrorCallback;
  onStatus: StatusCallback;
}