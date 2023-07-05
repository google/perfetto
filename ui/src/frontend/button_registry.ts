import { Registry } from '../common/registry';

export interface CustomButtonArgs {
    title: string;
    icon: string;
    callback: () => {};
}
export class CustomButton {
    title: string;
    icon: string;
    callback: () => {};
    kind: string;
    constructor(args: CustomButtonArgs) {
        this.title = args.title;
        this.icon = args.icon;
        this.callback = args.callback;
        this.kind = args.title;
    }
}
export const customButtonRegistry = Registry.kindRegistry<CustomButton>();
