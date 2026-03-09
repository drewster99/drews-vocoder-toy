# Making the video demo with Claude

I had this vocoder I was making for macOS and had generated the 3 audio files you see in this folder. I wanted to show them on the github page, but the best I'd be able to do by just including links to them was get them to download to the viewer's device. But, github handles video much more easily. So I needed a video.

## iMovie: Nope;  Claude: Yep!

I opened up iMovie and started creating a new project, but then closed it, deciding to see if Claude could take care of it for me.

## Claude prompt

Here's the actual prompt I gave claude, verbatim:

> OK, here's the plan. Look at the 3 audio files in this folder.
> 
> OK, we are going to make a video out of them.
> 
> We will first do built-in-pitch-track-chord, then scratchy-robot and finally manyvoice, in that order.
> 
> For the first 6 seconds, we want a banner centered horizontally and offset by 10% from the top of the screen, medium size, that says "(Unmute your sound to listen)", with 1 second visible, 1 second hidden, repeating.
> 
> The banner should have yellow background (rectangular) and black text bold, 24 point at least.
> 
> For each of the 3 sounds, we'll need:
> 
> 1. Show the name of each, like "Pitch Tracked Chord" or "Scratchy Robot" or "Many Voices Robot".
> The name should clearly visible and just below where the yellow unmute banner will be.
> 
> 2. Show a visualization while playing. It would be cool if it were based on the audio. We could show a waveform. The main thing is that it needs to LOOK cool.
> 
> 3. For each of the 3 sounds, we need a different colored background, or a different colored visualization if the visualization takes up a lot of the screen and is colorful.
> 
> The visualizations could either by all the style but different colors, or they could be 3 different styles (also with different colors).
> 
> Lastly, instead of making this directly, we should make scripts or python code to generate everything, so we can regenerate or iterate fairly easily.

## Claude's plan, in response

Here's [Claude's Plan](claudes_plan.md) that it made in response to my prompt.

## Results

Claude made the file [make_demo_video--original.py](make_demo_video--original.py), which it ran for me with `python3 make_demo_video--original.py`. This generated the output file, [vocoder_demo--original.mp4](vocoder_demo--original.mp4).

There seems to be a little audio "pop" as each audio file begins playback, and the first visualization seems a touch spastic, and I decided to add the repo URL to the video with a follow-up prompt:

## Follow-Up Prompt

> We need to make a few tweaks:
> 
> 1- when each audio starts playing, there is an audible "pop". I'm not sure if
> that's in the audio file itself or something we're doing. I'd like to handle it,
> though, without modifying the given audio files.
> 
> The other notes are just on visualizations: Generally, we need to make them more
> colorful. Details:
> 
> 2- the first visualization is a little spazzy - presumbaly it's updating every
> sample or something? Perhaps it could update slightly less often, maybe 10 to 15
> times per second, and show some kind of peaks with fallback. maybe a glow.
> something to make it a bit more colorful.
> 
> 3- for the 2nd visualization, we need to make it more colorful. make it pop! add
> glows or particles or something to jazz it up.
> 
> 4- for the 4th visualization, we've got the same sort of issue, but also, the
> waveform waves don't extended very far away from the vertical center of the screen,
>  so they leave a lot of black background, which is what we're hoping to avoid here
> with all of these. Maybe they can extend higher, and maybe little pulse waves could
>  come off of them or something?
> 
> 5- Lastly, I want to include the github URL in the lower right of the video, which
> is https://github.com/drewster99/drews-vocoder-toy. We should probably also put
> "Drew's Vocoder Toy" in the upper left. Also, for all the text, let's pick something
>  less boring than Ariel. Pick something cool and techy. Or refined. No ariel or
> helvetica please.

After sending this, it spent a fair amount of time looking at available fonts and eventually settled on Futura. It made its updates to the python script, generated the video, and the grabbed sample frames to look at them and assess. It decided that the 3rd visualization seemed a little dead. It analyzed the corresponding input audio file and decided that there was a lot of dead time in there (between words/speaking).

Then it repeated the process, building the video again and grabbing some frames to analzye. It then decided that its plan to normalize relative to the peak amplitude made everything else tiny if there was a single high spike. So it made a new plan and tried again.

## Final Python and Video

This left me with this [final python code](make_demo_video.py) and, of course, the [final demo video](vocoder_demo.mp4).
