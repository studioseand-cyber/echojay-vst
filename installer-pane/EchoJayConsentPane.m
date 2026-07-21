/*
  EchoJayConsentPane.m

  Custom Installer pane: the sharing-consent either/or. Shown after the
  package-selection step (see InstallerSections.plist). Two radio buttons,
  NEITHER pre-selected; Continue is disabled and shouldExitPane: returns NO
  until the user actively picks one. The pick writes mapping_consent.json
  directly (the pane runs as the console user inside Installer.app, so
  NSHomeDirectory() is the right home):

    Share        -> {"consent":"fetch-and-contribute"}
    Don't share  -> {"consent":"fetch-only"}

  The harness (ejextract --bootstrap) reads that marker; its contribute
  stage is hard-gated on fetch-and-contribute. The "Set up plugin
  auto-mapping" component stays an ordinary ticked-on checkbox in the
  package-selection pane; scan+fetch is read-only and needs no either/or.

  All UI is built programmatically in the contentView getter; the nib
  contains only the InstallerSection -> pane wiring.

  House style: no em-dashes.
*/

#import <Cocoa/Cocoa.h>
#import <InstallerPlugins/InstallerPlugins.h>

@interface EchoJayConsentPane : InstallerPane
@end

@implementation EchoJayConsentPane
{
    NSView   *_view;
    NSButton *_shareBtn;
    NSButton *_noShareBtn;
    BOOL      _picked;
}

- (NSString *)title
{
    return @"Data Sharing";
}

// Wrapping multi-line label for Auto Layout: width comes from the leading/
// trailing constraints, height from the wrapped text's intrinsic size.
static NSTextField *makeLabel (NSString *text, CGFloat size, BOOL bold)
{
    NSTextField *l = [NSTextField wrappingLabelWithString:text];
    l.font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
    l.selectable = NO;
    l.translatesAutoresizingMaskIntoConstraints = NO;
    [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    return l;
}

- (NSView *)contentView
{
    if (_view != nil) return _view;

    // Auto Layout throughout: the pane area's real size is Installer's
    // decision, so everything anchors to the TOP edge and pins leading +
    // trailing with a 20px inset. Labels wrap; nothing can clip on the
    // right or start scrolled out of view. Any spare space collects at the
    // bottom (no bottom constraint), which is the intended layout.
    _view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 620, 418)];

    NSTextField *heading = makeLabel(@"Help improve auto-dialing?", 16, YES);
    NSTextField *body = makeLabel(
        @"EchoJay can learn plugin controls from its users. Choose whether "
        @"to contribute your plugins' parameter data (control names and "
        @"ranges only, never audio, projects, or personal data). Auto-mapping "
        @"works fully either way.", 12, NO);

    // Same target+action makes AppKit treat the two as one radio group.
    _shareBtn = [NSButton radioButtonWithTitle:@"Share anonymized plugin data (recommended)"
                                        target:self action:@selector(picked:)];
    _noShareBtn = [NSButton radioButtonWithTitle:@"Don't share"
                                          target:self action:@selector(picked:)];
    _shareBtn.state = NSControlStateValueOff;
    _noShareBtn.state = NSControlStateValueOff;
    _shareBtn.translatesAutoresizingMaskIntoConstraints = NO;
    _noShareBtn.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField *shareCap = makeLabel(
        @"Grows the shared map database so more plugins can be auto-dialed, "
        @"for you and everyone else.", 11, NO);
    NSTextField *noShareCap = makeLabel(
        @"EchoJay fetches existing maps for your plugins and sends nothing "
        @"back. You can still use every feature.", 11, NO);
    shareCap.textColor = [NSColor secondaryLabelColor];
    noShareCap.textColor = [NSColor secondaryLabelColor];

    for (NSView *v in @[ heading, body, _shareBtn, shareCap, _noShareBtn, noShareCap ])
        [_view addSubview:v];

    [NSLayoutConstraint activateConstraints:@[
        // heading pinned to the TOP with 20px padding, full width minus insets
        [heading.topAnchor      constraintEqualToAnchor:_view.topAnchor constant:20],
        [heading.leadingAnchor  constraintEqualToAnchor:_view.leadingAnchor  constant:20],
        [heading.trailingAnchor constraintEqualToAnchor:_view.trailingAnchor constant:-20],

        [body.topAnchor      constraintEqualToAnchor:heading.bottomAnchor constant:12],
        [body.leadingAnchor  constraintEqualToAnchor:_view.leadingAnchor  constant:20],
        [body.trailingAnchor constraintEqualToAnchor:_view.trailingAnchor constant:-20],

        [_shareBtn.topAnchor      constraintEqualToAnchor:body.bottomAnchor constant:24],
        [_shareBtn.leadingAnchor  constraintEqualToAnchor:_view.leadingAnchor  constant:24],
        [_shareBtn.trailingAnchor constraintLessThanOrEqualToAnchor:_view.trailingAnchor constant:-20],

        // captions indented under their radio, wrapped, right-inset 20px
        [shareCap.topAnchor      constraintEqualToAnchor:_shareBtn.bottomAnchor constant:4],
        [shareCap.leadingAnchor  constraintEqualToAnchor:_view.leadingAnchor  constant:44],
        [shareCap.trailingAnchor constraintEqualToAnchor:_view.trailingAnchor constant:-20],

        // spacer between the two options
        [_noShareBtn.topAnchor      constraintEqualToAnchor:shareCap.bottomAnchor constant:20],
        [_noShareBtn.leadingAnchor  constraintEqualToAnchor:_view.leadingAnchor  constant:24],
        [_noShareBtn.trailingAnchor constraintLessThanOrEqualToAnchor:_view.trailingAnchor constant:-20],

        [noShareCap.topAnchor      constraintEqualToAnchor:_noShareBtn.bottomAnchor constant:4],
        [noShareCap.leadingAnchor  constraintEqualToAnchor:_view.leadingAnchor  constant:44],
        [noShareCap.trailingAnchor constraintEqualToAnchor:_view.trailingAnchor constant:-20],
    ]];

    return _view;
}

- (void)didEnterPane:(InstallerSectionDirection)dir
{
    // Continue stays disabled until a pick is made (belt); shouldExitPane:
    // below is the brace in case anything re-enables it.
    [self setNextEnabled:_picked];
}

- (void)picked:(id)sender
{
    _picked = YES;
    [self setNextEnabled:YES];
    [self writeMarker];   // written at pick AND at exit: survives odd exits
}

- (BOOL)shouldExitPane:(InstallerSectionDirection)dir
{
    if (dir != InstallerDirectionForward)
        return YES;                       // going back is always allowed
    if (! _picked)
        return NO;                        // HARD GATE: no pick, no Continue
    [self writeMarker];
    return YES;
}

- (void)writeMarker
{
    const BOOL share = (_shareBtn.state == NSControlStateValueOn);
    NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/EchoJay"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSISO8601DateFormatter *fmt = [NSISO8601DateFormatter new];
    NSString *json = [NSString stringWithFormat:
        @"{\"consent\":\"%@\",\"source\":\"installer-pane\",\"at\":\"%@\"}",
        share ? @"fetch-and-contribute" : @"fetch-only",
        [fmt stringFromDate:[NSDate date]]];
    [json writeToFile:[dir stringByAppendingPathComponent:@"mapping_consent.json"]
           atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSLog(@"EchoJayConsentPane: wrote consent %@", share ? @"fetch-and-contribute" : @"fetch-only");
}

@end
