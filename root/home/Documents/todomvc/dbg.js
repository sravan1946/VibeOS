setTimeout(function () {
    var a = document.querySelector('.todoapp'), r = a.getBoundingClientRect(), c = getComputedStyle(a);
    console.log('TODOAPP ' + r.x + ',' + r.y + ' ' + r.width + 'x' + r.height + ' disp=' + c.display + ' pos=' + c.position + ' kids=' + a.children.length +
        ' h1=' + document.querySelector('h1').textContent + ' items=' + document.querySelectorAll('.todo-list li').length);
}, 1500);
