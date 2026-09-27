import type { Metadata } from "next";
import QuartzGallery from "@/components/QuartzGallery";

export const metadata: Metadata = {
  title: "QUARTZ — light-LCD firmware deck",
  description:
    "The watch-face edition of the firmware deck: same sketches on a flat light LCD panel with ghost 7-seg digits and the red alarm sweep. Preview site.",
};

export default function QuartzPage() {
  return <QuartzGallery />;
}
