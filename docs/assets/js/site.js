// Traffic Engine - GitHub Pages site
// On <user>.github.io/<repository>/ the links with data-github go to
// github.com/<user>/<repository>, and the links with data-github-path go to
// that file of the main branch. On other hosts the links stay as they are.
(function ()
{
	var match = window.location.hostname.match(/^([^.]+)\.github\.io$/i);
	if (!match)
	{
		return;
	}
	var user = match[1];
	var parts = window.location.pathname.split('/').filter(function (p) { return p.length > 0; });
	var repository = parts.length > 0 && parts[0].indexOf('.') < 0 ? parts[0] : user + '.github.io';
	var base = 'https://github.com/' + user + '/' + repository;
	document.querySelectorAll('[data-github]').forEach(function (a)
	{
		a.href = base;
	});
	document.querySelectorAll('[data-github-path]').forEach(function (a)
	{
		a.href = base + '/blob/main/' + a.getAttribute('data-github-path');
	});
})();
